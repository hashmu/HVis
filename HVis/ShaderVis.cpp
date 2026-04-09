#include "ShaderVis.h"
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <windows.h>

// Fullscreen triangle vertex shader — no vertex buffer needed
static const char* g_vsFullscreen = R"(
void main(uint id : SV_VertexID,
          out float4 pos : SV_Position,
          out float2 uv : TEXCOORD0) {
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    uv.y = 1.0 - uv.y;
}
)";

// ============================================================
// File I/O helpers
// ============================================================

std::string ShaderVis::ReadTextFile(const std::string& path) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return {};
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string buf(sz, '\0');
    fread(&buf[0], 1, sz, f);
    fclose(f);
    return buf;
}

FILETIME ShaderVis::GetFileWriteTime(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA attr = {};
    GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attr);
    return attr.ftLastWriteTime;
}

std::string ShaderVis::FindShadersDir() {
    // Try several paths relative to working directory / exe
    const char* candidates[] = {
        "shaders",
        "../shaders",
        "../../shaders",
        "HVis/shaders",
    };
    for (auto c : candidates) {
        DWORD attr = GetFileAttributesA(c);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            return c;
        }
    }
    return {};
}

// ============================================================
// Shader compilation
// ============================================================

bool ShaderVis::CompilePixelShader(const char* hlsl, const char* name, ID3D11PixelShader** ps) {
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), name, nullptr, nullptr,
        "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            printf("[Shader] Compile error in '%s': %s\n", name, (char*)errors->GetBufferPointer());
            errors->Release();
        }
        return false;
    }
    if (errors) {
        printf("[Shader] Warning in '%s': %s\n", name, (char*)errors->GetBufferPointer());
        errors->Release();
    }

    hr = m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, ps);
    blob->Release();
    return SUCCEEDED(hr);
}

// ============================================================
// Directory scanning & shader loading
// ============================================================

bool ShaderVis::LoadShaderFile(const std::string& path, ShaderEntry& entry) {
    std::string hlsl = ReadTextFile(path);
    if (hlsl.empty()) {
        printf("[Shader] Failed to read: %s\n", path.c_str());
        return false;
    }

    // Derive display name from filename (strip directory and .hlsl extension)
    std::string name = path;
    size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    size_t dot = name.rfind(".hlsl");
    if (dot != std::string::npos) name = name.substr(0, dot);

    ID3D11PixelShader* ps = nullptr;
    if (!CompilePixelShader(hlsl.c_str(), name.c_str(), &ps))
        return false;

    entry.ps = ps;
    entry.name = name;
    entry.path = path;
    entry.lastWrite = GetFileWriteTime(path);
    return true;
}

void ShaderVis::ScanDirectory() {
    if (m_shaderDir.empty()) return;

    std::string pattern = m_shaderDir + "/*.hlsl";
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        std::string path = m_shaderDir + "/" + fd.cFileName;

        // Skip if already loaded
        bool found = false;
        for (auto& s : m_shaders) {
            if (s.path == path) { found = true; break; }
        }
        if (found) continue;

        ShaderEntry entry;
        if (LoadShaderFile(path, entry)) {
            printf("[Shader] Loaded: %s\n", entry.name.c_str());
            m_shaders.push_back(std::move(entry));
        }
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);

    // Sort by name for consistent ordering
    std::sort(m_shaders.begin(), m_shaders.end(),
        [](const ShaderEntry& a, const ShaderEntry& b) { return a.name < b.name; });
}

void ShaderVis::CheckHotReload() {
    if (m_shaderDir.empty()) return;

    // Check existing shaders for file changes
    for (int i = 0; i < (int)m_shaders.size(); i++) {
        auto& s = m_shaders[i];
        FILETIME ft = GetFileWriteTime(s.path);
        if (CompareFileTime(&ft, &s.lastWrite) <= 0) continue;

        // File changed — try to recompile
        std::string hlsl = ReadTextFile(s.path);
        if (hlsl.empty()) continue;

        ID3D11PixelShader* ps = nullptr;
        if (CompilePixelShader(hlsl.c_str(), s.name.c_str(), &ps)) {
            printf("[Shader] Hot-reloaded: %s\n", s.name.c_str());
            if (s.ps) s.ps->Release();
            s.ps = ps;
            s.lastWrite = ft;
        } else {
            // Compile failed — keep old shader, update timestamp so we don't spam
            s.lastWrite = ft;
        }
    }

    // Check for removed files
    for (int i = (int)m_shaders.size() - 1; i >= 0; i--) {
        DWORD attr = GetFileAttributesA(m_shaders[i].path.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            printf("[Shader] Removed: %s\n", m_shaders[i].name.c_str());
            if (m_shaders[i].ps) m_shaders[i].ps->Release();
            m_shaders.erase(m_shaders.begin() + i);
            if (m_currentShader >= (int)m_shaders.size())
                m_currentShader = m_shaders.empty() ? 0 : (int)m_shaders.size() - 1;
        }
    }

    // Check for new files
    ScanDirectory();
}

// ============================================================
// Init / Update / Render / Cleanup
// ============================================================

bool ShaderVis::Init(ID3D11Device* device, ID3D11DeviceContext* ctx) {
    m_device = device;
    m_ctx = ctx;

    // Compile vertex shader
    {
        ID3DBlob* blob = nullptr;
        ID3DBlob* errors = nullptr;
        HRESULT hr = D3DCompile(g_vsFullscreen, strlen(g_vsFullscreen), nullptr, nullptr, nullptr,
            "main", "vs_5_0", 0, 0, &blob, &errors);
        if (FAILED(hr)) {
            if (errors) { printf("[Shader] VS error: %s\n", (char*)errors->GetBufferPointer()); errors->Release(); }
            return false;
        }
        if (errors) errors->Release();
        hr = m_device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_vsFullscreen);
        blob->Release();
        if (FAILED(hr)) return false;
    }

    // Find shaders directory and load all .hlsl files
    m_shaderDir = FindShadersDir();
    if (m_shaderDir.empty()) {
        printf("[Shader] Could not find 'shaders' directory!\n");
        return false;
    }
    printf("[Shader] Using shader directory: %s\n", m_shaderDir.c_str());

    ScanDirectory();
    if (m_shaders.empty()) {
        printf("[Shader] No shaders found in %s\n", m_shaderDir.c_str());
        return false;
    }

    // Create constant buffer
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(ShaderCB);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_device->CreateBuffer(&bd, nullptr, &m_cbuffer))) return false;

    // Rasterizer state (no culling for fullscreen tri)
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    m_device->CreateRasterizerState(&rd, &m_rasterState);

    return true;
}

void ShaderVis::CreateOutputTexture(UINT width, UINT height) {
    if (m_outputSRV) { m_outputSRV->Release(); m_outputSRV = nullptr; }
    if (m_outputRTV) { m_outputRTV->Release(); m_outputRTV = nullptr; }
    if (m_outputTex) { m_outputTex->Release(); m_outputTex = nullptr; }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc = { 1, 0 };
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    m_device->CreateTexture2D(&td, nullptr, &m_outputTex);
    m_device->CreateRenderTargetView(m_outputTex, nullptr, &m_outputRTV);
    m_device->CreateShaderResourceView(m_outputTex, nullptr, &m_outputSRV);

    m_width = width;
    m_height = height;
}

void ShaderVis::Resize(UINT width, UINT height) {
    if (width == 0 || height == 0) return;
    if (width != m_width || height != m_height)
        CreateOutputTexture(width, height);
}

void ShaderVis::Update(float time, const AudioParams& audio) {
    // Hot reload check every RELOAD_INTERVAL frames
    if (++m_reloadCounter >= RELOAD_INTERVAL) {
        m_reloadCounter = 0;
        CheckHotReload();
    }

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m_ctx->Map(m_cbuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        ShaderCB* cb = (ShaderCB*)mapped.pData;
        cb->resolution[0] = (float)m_width;
        cb->resolution[1] = (float)m_height;
        cb->time = time;
        cb->bass = audio.bass;
        cb->mid = audio.mid;
        cb->treble = audio.treble;
        cb->energy = audio.energy;
        cb->pad = 0;
        memcpy(cb->bands, audio.bands, sizeof(float) * 32);
        m_ctx->Unmap(m_cbuffer, 0);
    }
}

void ShaderVis::Render() {
    if (!m_outputRTV || m_shaders.empty() || m_currentShader >= (int)m_shaders.size()) return;

    // Save current state
    ID3D11RenderTargetView* prevRTV = nullptr;
    ID3D11DepthStencilView* prevDSV = nullptr;
    D3D11_VIEWPORT prevVP;
    UINT numVP = 1;
    m_ctx->OMGetRenderTargets(1, &prevRTV, &prevDSV);
    m_ctx->RSGetViewports(&numVP, &prevVP);

    // Set our render target
    m_ctx->OMSetRenderTargets(1, &m_outputRTV, nullptr);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)m_width;
    vp.Height = (float)m_height;
    vp.MaxDepth = 1.0f;
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->RSSetState(m_rasterState);

    // Bind shaders and draw fullscreen triangle
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->IASetInputLayout(nullptr);
    m_ctx->VSSetShader(m_vsFullscreen, nullptr, 0);
    m_ctx->PSSetShader(m_shaders[m_currentShader].ps, nullptr, 0);
    m_ctx->PSSetConstantBuffers(0, 1, &m_cbuffer);
    m_ctx->Draw(3, 0);

    // Restore state
    m_ctx->OMSetRenderTargets(1, &prevRTV, prevDSV);
    m_ctx->RSSetViewports(1, &prevVP);
    if (prevRTV) prevRTV->Release();
    if (prevDSV) prevDSV->Release();
}

void ShaderVis::SetShader(int index) {
    if (index >= 0 && index < (int)m_shaders.size())
        m_currentShader = index;
}

const char* ShaderVis::GetShaderName(int index) const {
    if (index >= 0 && index < (int)m_shaders.size())
        return m_shaders[index].name.c_str();
    return "Unknown";
}

void ShaderVis::Cleanup() {
    if (m_outputSRV) { m_outputSRV->Release(); m_outputSRV = nullptr; }
    if (m_outputRTV) { m_outputRTV->Release(); m_outputRTV = nullptr; }
    if (m_outputTex) { m_outputTex->Release(); m_outputTex = nullptr; }
    if (m_cbuffer) { m_cbuffer->Release(); m_cbuffer = nullptr; }
    if (m_rasterState) { m_rasterState->Release(); m_rasterState = nullptr; }
    if (m_vsFullscreen) { m_vsFullscreen->Release(); m_vsFullscreen = nullptr; }
    for (auto& s : m_shaders) {
        if (s.ps) { s.ps->Release(); s.ps = nullptr; }
    }
    m_shaders.clear();
}
