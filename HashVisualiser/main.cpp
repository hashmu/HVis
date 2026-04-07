#include "AudioCapture.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <d3d11.h>
#include <tchar.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// D3D11 globals
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

// Forward declarations
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Audio
static AudioCapture g_audioCapture;
static const UINT32 AUDIO_BUFFER_SAMPLES = 48000 * 2; // 1 second stereo
static float g_audioBuffer[AUDIO_BUFFER_SAMPLES];

// Waveform ring buffer (mono, downmixed)
static const int WAVEFORM_SIZE = 2048;
static float g_waveform[WAVEFORM_SIZE] = {};
static int g_waveformPos = 0;

// FFT
static const int FFT_SIZE = 2048;
static float g_fftInput[FFT_SIZE] = {};
static float g_fftMagnitude[FFT_SIZE / 2] = {};
static float g_fftSmoothed[FFT_SIZE / 2] = {};

// Simple DFT for visualization (good enough for display, not perf-critical at 1024 bins)
static void ComputeFFT(const float* input, float* magnitude, int size) {
    int half = size / 2;
    for (int k = 0; k < half; k++) {
        float re = 0.0f, im = 0.0f;
        for (int n = 0; n < size; n++) {
            float angle = 2.0f * 3.14159265f * k * n / size;
            re += input[n] * cosf(angle);
            im -= input[n] * sinf(angle);
        }
        magnitude[k] = sqrtf(re * re + im * im) / size;
    }
}

// Hann window
static void ApplyHannWindow(float* data, int size) {
    for (int i = 0; i < size; i++) {
        float w = 0.5f * (1.0f - cosf(2.0f * 3.14159265f * i / (size - 1)));
        data[i] *= w;
    }
}

static void PollAudio() {
    UINT32 channels = g_audioCapture.GetChannels();
    if (channels == 0) return;

    UINT32 samplesRead = g_audioCapture.GetAudioData(g_audioBuffer, AUDIO_BUFFER_SAMPLES);
    if (samplesRead == 0) return;

    UINT32 frames = samplesRead / channels;
    for (UINT32 i = 0; i < frames; i++) {
        // Downmix to mono
        float mono = 0.0f;
        for (UINT32 c = 0; c < channels; c++) {
            mono += g_audioBuffer[i * channels + c];
        }
        mono /= channels;

        g_waveform[g_waveformPos] = mono;
        g_waveformPos = (g_waveformPos + 1) % WAVEFORM_SIZE;
    }

    // Fill FFT input from waveform ring buffer (most recent samples)
    for (int i = 0; i < FFT_SIZE; i++) {
        int idx = (g_waveformPos - FFT_SIZE + i + WAVEFORM_SIZE) % WAVEFORM_SIZE;
        g_fftInput[i] = g_waveform[idx];
    }
    ApplyHannWindow(g_fftInput, FFT_SIZE);
    ComputeFFT(g_fftInput, g_fftMagnitude, FFT_SIZE);

    // Smooth spectrum
    for (int i = 0; i < FFT_SIZE / 2; i++) {
        g_fftSmoothed[i] = g_fftSmoothed[i] * 0.7f + g_fftMagnitude[i] * 0.3f;
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    // Create window
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance,
        nullptr, nullptr, nullptr, nullptr, L"HashVisualiser", nullptr };
    ::RegisterClassExW(&wc);

    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"Hash Visualiser",
        WS_OVERLAPPEDWINDOW, 100, 100, 1280, 720,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 2.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.08f, 1.0f);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Initialize audio
    bool audioOk = g_audioCapture.Initialize(true);
    if (audioOk) audioOk = g_audioCapture.Start();

    const float clearColor[4] = { 0.04f, 0.04f, 0.06f, 1.0f };

    // Main loop
    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // Poll audio
        if (audioOk) PollAudio();

        // Start ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Fullscreen window
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##Main", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        float windowW = ImGui::GetContentRegionAvail().x;
        float windowH = ImGui::GetContentRegionAvail().y;

        // Title
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Hash Visualiser");
        ImGui::SameLine(windowW - 200);
        if (audioOk) {
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "WASAPI Loopback: %dHz %dch",
                g_audioCapture.GetSampleRate(), g_audioCapture.GetChannels());
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Audio: Not Available");
        }
        ImGui::Separator();

        float panelH = (windowH - 40) * 0.5f;

        // --- Waveform ---
        ImGui::Text("Waveform");
        {
            ImVec2 pos = ImGui::GetCursorScreenPos();
            ImVec2 size(windowW, panelH - 30);
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Background
            dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                IM_COL32(10, 10, 15, 255));

            // Center line
            float cy = pos.y + size.y * 0.5f;
            dl->AddLine(ImVec2(pos.x, cy), ImVec2(pos.x + size.x, cy),
                IM_COL32(40, 40, 60, 255));

            // Draw waveform
            int step = WAVEFORM_SIZE / (int)size.x;
            if (step < 1) step = 1;
            float prevY = cy;
            for (int i = 0; i < (int)size.x; i++) {
                int idx = (g_waveformPos + (i * WAVEFORM_SIZE / (int)size.x)) % WAVEFORM_SIZE;
                float sample = g_waveform[idx];
                float y = cy - sample * size.y * 0.45f;

                if (i > 0) {
                    // Color based on amplitude
                    float amp = fabsf(sample);
                    int r = (int)(80 + amp * 175);
                    int g = (int)(180 - amp * 80);
                    int b = (int)(255 - amp * 100);
                    dl->AddLine(ImVec2(pos.x + i - 1, prevY), ImVec2(pos.x + i, y),
                        IM_COL32(r, g, b, 255), 1.5f);
                }
                prevY = y;
            }

            // Border
            dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                IM_COL32(60, 60, 80, 255));

            ImGui::Dummy(size);
        }

        ImGui::Spacing();

        // --- Spectrum ---
        ImGui::Text("Spectrum");
        {
            ImVec2 pos = ImGui::GetCursorScreenPos();
            ImVec2 size(windowW, panelH - 30);
            ImDrawList* dl = ImGui::GetWindowDrawList();

            dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                IM_COL32(10, 10, 15, 255));

            // Draw spectrum bars (logarithmic frequency grouping)
            int numBars = 128;
            float barW = size.x / numBars;
            int halfFFT = FFT_SIZE / 2;

            for (int i = 0; i < numBars; i++) {
                // Log-scale frequency mapping
                float t = (float)i / numBars;
                int binStart = (int)(powf(t, 2.0f) * halfFFT);
                int binEnd = (int)(powf((float)(i + 1) / numBars, 2.0f) * halfFFT);
                if (binEnd <= binStart) binEnd = binStart + 1;
                if (binEnd > halfFFT) binEnd = halfFFT;

                // Average bins in range
                float mag = 0.0f;
                int count = 0;
                for (int b = binStart; b < binEnd; b++) {
                    mag += g_fftSmoothed[b];
                    count++;
                }
                if (count > 0) mag /= count;

                // Scale for visibility
                float dB = 20.0f * log10f(mag + 1e-6f);
                float norm = (dB + 60.0f) / 60.0f; // -60dB to 0dB range
                if (norm < 0.0f) norm = 0.0f;
                if (norm > 1.0f) norm = 1.0f;

                float barH = norm * size.y * 0.95f;
                float x = pos.x + i * barW;
                float y = pos.y + size.y - barH;

                // Gradient color: blue -> cyan -> green -> yellow -> red
                int r, g, b;
                if (norm < 0.5f) {
                    float f = norm * 2.0f;
                    r = (int)(30 * f);
                    g = (int)(100 + 155 * f);
                    b = (int)(255 - 100 * f);
                } else {
                    float f = (norm - 0.5f) * 2.0f;
                    r = (int)(30 + 225 * f);
                    g = (int)(255 - 155 * f);
                    b = (int)(155 - 155 * f);
                }

                dl->AddRectFilled(ImVec2(x + 1, y), ImVec2(x + barW - 1, pos.y + size.y),
                    IM_COL32(r, g, b, 220));
            }

            dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                IM_COL32(60, 60, 80, 255));

            ImGui::Dummy(size);
        }

        ImGui::End();

        // Render
        ImGui::Render();
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0); // VSync
    }

    // Cleanup
    g_audioCapture.Cleanup();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}

// --- D3D11 helpers ---

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate = { 60, 1 };
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc = { 1, 0 };
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        0, levels, 1, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
        &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (FAILED(hr)) return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    g_pd3dDevice->CreateRenderTargetView(backBuffer, nullptr, &g_mainRenderTargetView);
    backBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam),
                DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
