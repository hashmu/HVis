#include "AudioCapture.h"
#include "ShaderVis.h"
#include "PostProcess.h"

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
void RenderFrame();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Render thread state
static HANDLE g_renderThread = nullptr;
static volatile bool g_renderThreadRunning = false;
static CRITICAL_SECTION g_resizeCS;
static volatile bool g_needResize = false;
static UINT g_resizeWidth = 0;
static UINT g_resizeHeight = 0;

// Message queue — WndProc pushes, render thread replays for ImGui
struct QueuedMsg { HWND hwnd; UINT msg; WPARAM wParam; LPARAM lParam; };
static const int MSG_QUEUE_SIZE = 256;
static QueuedMsg g_msgQueue[MSG_QUEUE_SIZE];
static volatile LONG g_msgQueueHead = 0; // written by main thread
static volatile LONG g_msgQueueTail = 0; // read by render thread
static CRITICAL_SECTION g_msgCS;

static void QueueWinMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    EnterCriticalSection(&g_msgCS);
    LONG next = (g_msgQueueHead + 1) % MSG_QUEUE_SIZE;
    if (next != g_msgQueueTail) { // drop if full
        g_msgQueue[g_msgQueueHead] = { hwnd, msg, wParam, lParam };
        g_msgQueueHead = next;
    }
    LeaveCriticalSection(&g_msgCS);
}

static void DrainMessageQueue() {
    EnterCriticalSection(&g_msgCS);
    while (g_msgQueueTail != g_msgQueueHead) {
        QueuedMsg& m = g_msgQueue[g_msgQueueTail];
        ImGui_ImplWin32_WndProcHandler(m.hwnd, m.msg, m.wParam, m.lParam);
        g_msgQueueTail = (g_msgQueueTail + 1) % MSG_QUEUE_SIZE;
    }
    LeaveCriticalSection(&g_msgCS);
}

static bool g_audioOk = false;
static ShaderVis g_shaderVis;
static float g_time = 0.0f;
static LARGE_INTEGER g_perfFreq = {};
static LARGE_INTEGER g_perfStart = {};

// Vis mode: 0 = waveform/spectrum, 1+ = shader modes
static int g_visMode = 0;

// Audio analysis output
static AudioParams g_audioParams = {};

static PostProcess g_postProcess;
static PostProcessSettings g_ppSettings;
static bool g_showPostFX = false;

// AGC (automatic gain control) — per-band peak tracking
static float g_bandPeak[32] = {};       // running peak per band
static float g_bassPeak = 0.001f;
static float g_midPeak = 0.001f;
static float g_treblePeak = 0.001f;

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

    // Smooth spectrum — faster attack, slower release (good for transients)
    for (int i = 0; i < FFT_SIZE / 2; i++) {
        float raw = g_fftMagnitude[i];
        if (raw > g_fftSmoothed[i])
            g_fftSmoothed[i] = g_fftSmoothed[i] * 0.3f + raw * 0.7f;  // fast attack
        else
            g_fftSmoothed[i] = g_fftSmoothed[i] * 0.85f + raw * 0.15f; // slow release
    }

    // --- Perceptual frequency weighting (approximate A-weighting) ---
    // Higher frequencies have less raw FFT energy but are perceptually important.
    // This curve boosts upper mids/treble to compensate.
    int halfFFT = FFT_SIZE / 2;
    float sampleRate = (float)g_audioCapture.GetSampleRate();
    if (sampleRate == 0) sampleRate = 48000;

    // Weighted spectrum for analysis
    static float weighted[FFT_SIZE / 2];
    for (int i = 0; i < halfFFT; i++) {
        float freq = (float)i * sampleRate / FFT_SIZE;
        // A-weight approximation: boost 1k-6k, roll off sub-bass and ultra-high
        float w = 1.0f;
        if (freq < 200.0f)       w = 0.5f + 0.5f * (freq / 200.0f);       // gentle sub-bass rolloff
        else if (freq < 1000.0f) w = 1.0f;                                  // flat low-mid
        else if (freq < 4000.0f) w = 1.0f + 1.5f * ((freq - 1000.0f) / 3000.0f); // boost presence
        else if (freq < 8000.0f) w = 2.5f;                                  // presence peak
        else                     w = 2.5f - 1.0f * ((freq - 8000.0f) / 12000.0f); // gentle air rolloff
        if (w < 0.3f) w = 0.3f;
        weighted[i] = g_fftSmoothed[i] * w;
    }

    // --- Bass / Mid / Treble with AGC ---
    int bassBin = (int)(300.0f / sampleRate * FFT_SIZE);
    int midBin  = (int)(2000.0f / sampleRate * FFT_SIZE);

    float bassRaw = 0, midRaw = 0, trebleRaw = 0;
    for (int i = 0; i < halfFFT; i++) {
        if (i < bassBin) bassRaw += weighted[i];
        else if (i < midBin) midRaw += weighted[i];
        else trebleRaw += weighted[i];
    }
    bassRaw   /= (bassBin > 0 ? bassBin : 1);
    midRaw    /= (midBin - bassBin > 0 ? midBin - bassBin : 1);
    trebleRaw /= (halfFFT - midBin > 0 ? halfFFT - midBin : 1);

    // AGC: track peaks with slow decay, normalize to 0-1 range
    float agcDecay = 0.998f;  // ~5 second half-life at 60fps
    float agcFloor = 0.001f;  // minimum peak to avoid division by near-zero
    g_bassPeak   = fmaxf(fmaxf(g_bassPeak * agcDecay, bassRaw), agcFloor);
    g_midPeak    = fmaxf(fmaxf(g_midPeak * agcDecay, midRaw), agcFloor);
    g_treblePeak = fmaxf(fmaxf(g_treblePeak * agcDecay, trebleRaw), agcFloor);

    float bassNorm   = bassRaw / g_bassPeak;
    float midNorm    = midRaw / g_midPeak;
    float trebleNorm = trebleRaw / g_treblePeak;

    // Per-band smoothing: bass = smooth, mid = medium, treble = snappy
    g_audioParams.bass   = g_audioParams.bass * 0.75f + bassNorm * 0.25f;
    g_audioParams.mid    = g_audioParams.mid * 0.6f + midNorm * 0.4f;
    g_audioParams.treble = g_audioParams.treble * 0.4f + trebleNorm * 0.6f;
    g_audioParams.energy = (g_audioParams.bass + g_audioParams.mid + g_audioParams.treble) / 3.0f;

    // --- 32 bands: log-spaced, perceptually weighted, with per-band AGC ---
    for (int i = 0; i < 32; i++) {
        float t0 = (float)i / 32.0f;
        float t1 = (float)(i + 1) / 32.0f;
        int b0 = (int)(t0 * t0 * halfFFT);
        int b1 = (int)(t1 * t1 * halfFFT);
        if (b1 <= b0) b1 = b0 + 1;
        if (b1 > halfFFT) b1 = halfFFT;

        float sum = 0;
        for (int b = b0; b < b1; b++) sum += weighted[b];
        float avg = sum / (b1 - b0);

        // Per-band AGC
        g_bandPeak[i] = fmaxf(fmaxf(g_bandPeak[i] * agcDecay, avg), agcFloor);
        float norm = avg / g_bandPeak[i];

        // Adaptive smoothing: lower bands smoother, upper bands snappier
        float smooth = 0.8f - (float)i * 0.015f; // band 0 = 0.8, band 31 = 0.33
        if (smooth < 0.3f) smooth = 0.3f;

        if (norm > g_audioParams.bands[i])
            g_audioParams.bands[i] = g_audioParams.bands[i] * (smooth * 0.5f) + norm * (1.0f - smooth * 0.5f);
        else
            g_audioParams.bands[i] = g_audioParams.bands[i] * smooth + norm * (1.0f - smooth);
    }
}

static void DrawWaveformSpectrum(float windowW, float windowH) {
    float panelH = (windowH - 40) * 0.5f;

    // --- Waveform ---
    ImGui::Text("Waveform");
    {
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImVec2 size(windowW, panelH - 30);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
            IM_COL32(10, 10, 15, 255));

        float cy = pos.y + size.y * 0.5f;
        dl->AddLine(ImVec2(pos.x, cy), ImVec2(pos.x + size.x, cy),
            IM_COL32(40, 40, 60, 255));

        float prevY = cy;
        for (int i = 0; i < (int)size.x; i++) {
            int idx = (g_waveformPos + (i * WAVEFORM_SIZE / (int)size.x)) % WAVEFORM_SIZE;
            float sample = g_waveform[idx];
            float y = cy - sample * size.y * 0.45f;

            if (i > 0) {
                float amp = fabsf(sample);
                int r = (int)(80 + amp * 175);
                int g = (int)(180 - amp * 80);
                int b = (int)(255 - amp * 100);
                dl->AddLine(ImVec2(pos.x + i - 1, prevY), ImVec2(pos.x + i, y),
                    IM_COL32(r, g, b, 255), 1.5f);
            }
            prevY = y;
        }

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

        int numBars = 128;
        float barW = size.x / numBars;
        int halfFFT = FFT_SIZE / 2;

        for (int i = 0; i < numBars; i++) {
            float t = (float)i / numBars;
            int binStart = (int)(powf(t, 2.0f) * halfFFT);
            int binEnd = (int)(powf((float)(i + 1) / numBars, 2.0f) * halfFFT);
            if (binEnd <= binStart) binEnd = binStart + 1;
            if (binEnd > halfFFT) binEnd = halfFFT;

            float mag = 0.0f;
            int count = 0;
            for (int b = binStart; b < binEnd; b++) {
                mag += g_fftSmoothed[b];
                count++;
            }
            if (count > 0) mag /= count;

            float dB = 20.0f * log10f(mag + 1e-6f);
            float norm = (dB + 60.0f) / 60.0f;
            if (norm < 0.0f) norm = 0.0f;
            if (norm > 1.0f) norm = 1.0f;

            float barH = norm * size.y * 0.95f;
            float x = pos.x + i * barW;
            float y = pos.y + size.y - barH;

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
}

void RenderFrame() {
    // Handle pending resize
    EnterCriticalSection(&g_resizeCS);
    if (g_needResize) {
        CleanupRenderTarget();
        g_pSwapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
        CreateRenderTarget();
        g_needResize = false;
    }
    LeaveCriticalSection(&g_resizeCS);

    // Update time
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_time = (float)(now.QuadPart - g_perfStart.QuadPart) / (float)g_perfFreq.QuadPart;

    if (g_audioOk) PollAudio();

    // Update shader vis
    g_shaderVis.Update(g_time, g_audioParams);

    DrainMessageQueue();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGuiIO& io = ImGui::GetIO();

    // Fullscreen window
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##Main", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    float windowW = ImGui::GetContentRegionAvail().x;
    float windowH = ImGui::GetContentRegionAvail().y;

    // Header bar
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "HVis");

    // Vis mode tabs
    ImGui::SameLine(160);
    if (ImGui::SmallButton("Waveform/Spectrum")) g_visMode = 0;
    for (int i = 0; i < g_shaderVis.GetShaderCount(); i++) {
        ImGui::SameLine();
        if (ImGui::SmallButton(g_shaderVis.GetShaderName(i))) {
            g_visMode = 1 + i;
            g_shaderVis.SetShader(i);
        }
    }

    ImGui::SameLine();
    if (g_visMode > 0) {
        if (ImGui::SmallButton(g_showPostFX ? "PostFX [-]" : "PostFX [+]"))
            g_showPostFX = !g_showPostFX;
    }

    ImGui::SameLine(windowW - 200);
    if (g_audioOk) {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "WASAPI: %dHz %dch",
            g_audioCapture.GetSampleRate(), g_audioCapture.GetChannels());
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Audio: N/A");
    }
    ImGui::Separator();

    if (g_visMode == 0) {
        DrawWaveformSpectrum(windowW, windowH);
    } else {
        // Shader visualization + post-processing
        ImVec2 avail = ImGui::GetContentRegionAvail();
        UINT texW = (UINT)avail.x;
        UINT texH = (UINT)avail.y;
        if (texW > 0 && texH > 0) {
            g_shaderVis.Resize(texW, texH);
            g_shaderVis.Render();

            // Post-process pass
            g_postProcess.Resize(texW, texH);
            g_postProcess.Apply(g_shaderVis.GetOutputSRV(), g_time,
                g_audioParams.bass, g_audioParams.mid,
                g_audioParams.treble, g_audioParams.energy, g_ppSettings);

            ID3D11ShaderResourceView* srv = g_postProcess.GetOutputSRV();
            if (srv)
                ImGui::Image((ImTextureID)srv, avail);
        }
    }

    ImGui::End();

    // --- PostFX settings window ---
    if (g_showPostFX && g_visMode > 0) {
        ImGui::SetNextWindowSize(ImVec2(320, 520), ImGuiCond_FirstUseEver);
        ImGui::Begin("Post Processing", &g_showPostFX);

        PostProcessSettings& s = g_ppSettings;

        if (ImGui::CollapsingHeader("Bloom", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enable##bloom", &s.bloom);
            ImGui::SliderFloat("Intensity##bloom", &s.bloomIntensity, 0.0f, 2.0f);
            ImGui::SliderFloat("Threshold##bloom", &s.bloomThreshold, 0.0f, 1.0f);
            ImGui::Checkbox("Audio Reactive##bloom", &s.bloomAudioReactive);
        }

        if (ImGui::CollapsingHeader("Chromatic Aberration")) {
            ImGui::Checkbox("Enable##ca", &s.chromaticAberration);
            ImGui::SliderFloat("Intensity##ca", &s.caIntensity, 0.0f, 0.02f);
            ImGui::Checkbox("Audio Reactive##ca", &s.caAudioReactive);
        }

        if (ImGui::CollapsingHeader("Feedback Trails")) {
            ImGui::Checkbox("Enable##fb", &s.feedback);
            ImGui::SliderFloat("Trail Amount##fb", &s.feedbackAmount, 0.5f, 0.99f);
            ImGui::Checkbox("Audio Reactive##fb", &s.feedbackAudioReactive);
        }

        if (ImGui::CollapsingHeader("Radial Blur")) {
            ImGui::Checkbox("Enable##rb", &s.radialBlur);
            ImGui::SliderFloat("Intensity##rb", &s.radialBlurIntensity, 0.0f, 0.1f);
            ImGui::Checkbox("Audio Reactive##rb", &s.radialBlurAudioReactive);
        }

        if (ImGui::CollapsingHeader("Film Grain")) {
            ImGui::Checkbox("Enable##grain", &s.filmGrain);
            ImGui::SliderFloat("Intensity##grain", &s.grainIntensity, 0.0f, 0.3f);
            ImGui::Checkbox("Audio Reactive##grain", &s.grainAudioReactive);
        }

        if (ImGui::CollapsingHeader("Scanlines")) {
            ImGui::Checkbox("Enable##scan", &s.scanlines);
            ImGui::SliderFloat("Intensity##scan", &s.scanlineIntensity, 0.0f, 0.5f);
        }

        if (ImGui::CollapsingHeader("Vignette")) {
            ImGui::Checkbox("Enable##vig", &s.vignette);
            ImGui::SliderFloat("Intensity##vig", &s.vignetteIntensity, 0.0f, 3.0f);
        }

        if (ImGui::CollapsingHeader("Color Grading")) {
            ImGui::Checkbox("Enable##cg", &s.colorGrading);
            ImGui::SliderFloat("Temperature", &s.temperature, -1.0f, 1.0f);
            ImGui::SliderFloat("Contrast", &s.contrast, 0.5f, 2.0f);
            ImGui::SliderFloat("Saturation", &s.saturation, 0.0f, 2.0f);
        }

        ImGui::Separator();
        if (ImGui::Button("Save Settings"))
            g_ppSettings.Save("postfx.cfg");
        ImGui::SameLine();
        if (ImGui::Button("Reset Defaults")) {
            g_ppSettings = PostProcessSettings();
            g_ppSettings.Save("postfx.cfg");
        }

        ImGui::End();

        // Auto-save when window is closed
        if (!g_showPostFX)
            g_ppSettings.Save("postfx.cfg");
    }

    // Render
    ImGui::Render();
    const float clearColor[4] = { 0.04f, 0.04f, 0.06f, 1.0f };
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    g_pSwapChain->Present(1, 0);
}

static DWORD WINAPI RenderThreadProc(LPVOID) {
    while (g_renderThreadRunning) {
        RenderFrame();
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    // Init critical sections before anything that can trigger WndProc
    InitializeCriticalSection(&g_resizeCS);
    InitializeCriticalSection(&g_msgCS);

    // Create window
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance,
        nullptr, nullptr, nullptr, nullptr, L"HVis", nullptr };
    ::RegisterClassExW(&wc);

    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"HVis",
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

    // Init timer
    QueryPerformanceFrequency(&g_perfFreq);
    QueryPerformanceCounter(&g_perfStart);

    // Init shader visualisations
    g_shaderVis.Init(g_pd3dDevice, g_pd3dDeviceContext);
    g_postProcess.Init(g_pd3dDevice, g_pd3dDeviceContext);
    g_ppSettings.Load("postfx.cfg");

    // Initialize audio
    g_audioOk = g_audioCapture.Initialize(true);
    if (g_audioOk) g_audioOk = g_audioCapture.Start();

    // Start render thread
    g_renderThreadRunning = true;
    g_renderThread = CreateThread(nullptr, 0, RenderThreadProc, nullptr, 0, nullptr);

    // Main loop — just pumps messages, rendering happens on the other thread
    MSG msg;
    while (::GetMessage(&msg, nullptr, 0, 0)) {
        ::TranslateMessage(&msg);
        ::DispatchMessage(&msg);
    }

    // Stop render thread
    g_renderThreadRunning = false;
    WaitForSingleObject(g_renderThread, INFINITE);
    CloseHandle(g_renderThread);
    DeleteCriticalSection(&g_resizeCS);
    DeleteCriticalSection(&g_msgCS);

    // Cleanup
    g_postProcess.Cleanup();
    g_shaderVis.Cleanup();
    g_audioCapture.Cleanup();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
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
    // Queue input messages for the render thread to process
    QueueWinMessage(hWnd, msg, wParam, lParam);

    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
            // Signal the render thread to do the resize
            EnterCriticalSection(&g_resizeCS);
            g_resizeWidth = (UINT)LOWORD(lParam);
            g_resizeHeight = (UINT)HIWORD(lParam);
            g_needResize = true;
            LeaveCriticalSection(&g_resizeCS);
        }
        return 0;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
