#include "ShaderVis.h"
#include <stdio.h>
#include <string.h>

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
// Shader: Fractal Dreamscape (Mandelbrot/Julia hybrid)
// ============================================================
static const char* g_psFractal = R"(
cbuffer CB : register(b0) {
    float2 resolution;
    float time;
    float bass;
    float mid;
    float treble;
    float energy;
    float pad;
    float bands[32];
};

float3 pal(float t, float3 a, float3 b, float3 c, float3 d) {
    return a + b * cos(6.28318 * (c * t + d));
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 p = (uv - 0.5) * float2(resolution.x / resolution.y, 1.0);

    // Misiurewicz points — guaranteed boundary at every scale
    static const float2 targets[5] = {
        float2(-0.7463,  0.1102),   // Seahorse valley pinch point
        float2(-0.1011,  0.9563),   // Misiurewicz M(3,1)
        float2(-0.1528,  1.0397),   // Period-3 bulb junction
        float2(-1.4012,  0.0000),   // Antenna tip
        float2(-0.7453,  0.1127),   // Seahorse double spiral
    };

    // --- Infinite zoom: always forward, crossfade at precision limit ---
    // Cycles overlap: the next layer starts zooming fadeZone before the current ends,
    // so when the current cycle wraps, the next layer is already at fadeZone — which
    // becomes the new cycle's starting logZoom. Stride = maxLogZoom - fadeZone.
    float maxLogZoom = 9.0;         // ~8000x mag, safe for float32
    float fadeZone = 1.5;           // overlap region
    float stride = maxLogZoom - fadeZone; // effective distance per cycle

    float totalPhase = time * 0.4;  // constant base zoom speed
    float cyclePhase = fmod(totalPhase, stride);
    int targetIdx = ((int)(totalPhase / stride)) % 5;
    int nextIdx = (targetIdx + 1) % 5;

    // Current layer: starts at fadeZone (picked up from previous crossfade), goes to maxLogZoom
    float logZoom = fadeZone + cyclePhase;
    float zoom = exp(-logZoom);

    // Linear progress through the fade zone (0→1), used for both zoom and blend
    float fadeLin = saturate((logZoom - (maxLogZoom - fadeZone)) / fadeZone);
    // Smooth version only for the visual blend opacity
    float fadeT = smoothstep(0.0, 1.0, fadeLin);

    // Audio drives rotation, color, and orbit trap animation
    float angle = time * 0.06 + bass * 0.5 + mid * 0.3;
    float ca = cos(angle), sa = sin(angle);
    p = float2(p.x * ca - p.y * sa, p.x * sa + p.y * ca);

    // --- Render current target ---
    float2 target = targets[targetIdx];
    target.x += sin(time * 0.2) * mid * 0.0003 * zoom;
    target.y += cos(time * 0.17) * bass * 0.0003 * zoom;
    float2 c1 = target + p * zoom;

    // --- Render next target (zooming in at same constant rate) ---
    float nextLogZoom = fadeLin * fadeZone;
    float nextZoom = exp(-nextLogZoom);
    float2 nextTarget = targets[nextIdx];
    nextTarget.x += sin(time * 0.2) * mid * 0.0003 * nextZoom;
    nextTarget.y += cos(time * 0.17) * bass * 0.0003 * nextZoom;
    float2 c2 = nextTarget + p * nextZoom;

    // Iterations scale with depth
    float depthT = saturate(logZoom / maxLogZoom);
    int maxIter = (int)lerp(150.0, 500.0, depthT) + (int)(treble * 50.0);
    maxIter = clamp(maxIter, 150, 600);

    int nextMaxIter = 200;

    // --- Iterate current ---
    float2 z = float2(0.0, 0.0);
    float smoothIter = 0.0;
    float minDist = 1e10;
    float minDistAx = 1e10;
    // Animated orbit trap — audio controls trap position
    float2 trapCenter = float2(
        sin(time * 0.15 + bass) * 0.3,
        cos(time * 0.12 + mid) * 0.3
    );
    int i;
    for (i = 0; i < maxIter; i++) {
        z = float2(z.x * z.x - z.y * z.y + c1.x, 2.0 * z.x * z.y + c1.y);
        minDist = min(minDist, length(z - trapCenter));
        minDistAx = min(minDistAx, min(abs(z.x), abs(z.y)));
        if (dot(z, z) > 65536.0) break;
    }
    if (i < maxIter)
        smoothIter = (float)i + 1.0 - log2(log2(dot(z, z))) + 4.0;

    // --- Iterate next (only during crossfade) ---
    float smoothIter2 = 0.0;
    float minDist2 = 1e10;
    float minDistAx2 = 1e10;
    int i2 = nextMaxIter;
    if (fadeT > 0.001) {
        float2 z2 = float2(0.0, 0.0);
        for (i2 = 0; i2 < nextMaxIter; i2++) {
            z2 = float2(z2.x * z2.x - z2.y * z2.y + c2.x, 2.0 * z2.x * z2.y + c2.y);
            minDist2 = min(minDist2, length(z2 - trapCenter));
            minDistAx2 = min(minDistAx2, min(abs(z2.x), abs(z2.y)));
            if (dot(z2, z2) > 65536.0) break;
        }
        if (i2 < nextMaxIter)
            smoothIter2 = (float)i2 + 1.0 - log2(log2(dot(z2, z2))) + 4.0;
    }

    // --- Coloring (shared palette logic, zoom-independent) ---
    float palBlend = 0.5 + 0.4 * sin(time * 0.15 + bass * 2.0);
    float trapMix = 0.2 + 0.3 * sin(time * 0.2 + treble);

    // Use time-based color cycling only (no zoom-dependent iterScale)
    // This keeps colors continuous across the crossfade boundary
    float t1 = smoothIter * 0.035 + time * 0.15;
    float trap1 = saturate(1.0 - minDist * 0.4);
    float trapAx1 = saturate(1.0 - minDistAx * 3.0);

    float3 colA = pal(t1,
        float3(0.5, 0.5, 0.5), float3(0.5, 0.5, 0.5),
        float3(1.0, 0.7, 0.4), float3(0.00, 0.15, 0.20));
    float3 colB = pal(t1 + 0.5,
        float3(0.5, 0.5, 0.5), float3(0.5, 0.5, 0.5),
        float3(2.0, 1.0, 0.0), float3(0.50, 0.20, 0.25));
    float3 iterCol = lerp(colA, colB, palBlend);

    float3 trapCol = pal(trap1 + time * 0.08,
        float3(0.5, 0.5, 0.5), float3(0.5, 0.5, 0.5),
        float3(1.0, 1.0, 1.0), float3(0.30, 0.20, 0.20));

    float3 col;
    if (i < maxIter) {
        col = lerp(iterCol, trapCol * trap1, trapMix);
        col += trapAx1 * float3(0.15, 0.1, 0.3) * (0.5 + bass * 0.5);
    } else {
        // Interior: use final z position and orbit trap data for structure
        float zLen = length(z);
        float zAngle = atan2(z.y, z.x);
        float trapGlow = exp(-minDist * 0.5);
        float axGlow = exp(-minDistAx * 3.0);

        // Final z creates basin-like patterns
        float3 inner = pal(zAngle * 0.3 + zLen * 0.5 + time * 0.08,
            float3(0.5, 0.5, 0.5), float3(0.5, 0.5, 0.5),
            float3(0.8, 0.6, 1.0), float3(0.20, 0.10, 0.30));

        // Orbit trap creates filament glow
        float3 trapInner = pal(minDist * 2.0 + time * 0.1,
            float3(0.5, 0.5, 0.5), float3(0.5, 0.5, 0.5),
            float3(1.0, 0.7, 0.4), float3(0.00, 0.33, 0.67));

        col = inner * 0.15 * (0.3 + energy * 0.7)
            + trapInner * trapGlow * 0.4
            + axGlow * float3(0.1, 0.05, 0.25) * (0.3 + bass * 0.4);
    }

    // --- Color the next target (same palette logic) ---
    float t2 = smoothIter2 * 0.035 + time * 0.15;
    float trap2val = saturate(1.0 - minDist2 * 0.4);

    float3 colA2 = pal(t2,
        float3(0.5, 0.5, 0.5), float3(0.5, 0.5, 0.5),
        float3(1.0, 0.7, 0.4), float3(0.00, 0.15, 0.20));
    float3 colB2 = pal(t2 + 0.5,
        float3(0.5, 0.5, 0.5), float3(0.5, 0.5, 0.5),
        float3(2.0, 1.0, 0.0), float3(0.50, 0.20, 0.25));
    float3 iterCol2 = lerp(colA2, colB2, palBlend);
    float3 trapCol2 = pal(trap2val + time * 0.08,
        float3(0.5, 0.5, 0.5), float3(0.5, 0.5, 0.5),
        float3(1.0, 1.0, 1.0), float3(0.30, 0.20, 0.20));
    float3 col2;
    if (i2 < nextMaxIter) {
        col2 = lerp(iterCol2, trapCol2 * trap2val, trapMix);
        col2 += saturate(1.0 - minDistAx2 * 3.0) * float3(0.15, 0.1, 0.3) * (0.5 + bass * 0.5);
    } else {
        float trapGlow2 = exp(-minDist2 * 0.5);
        float axGlow2 = exp(-minDistAx2 * 3.0);
        float3 trapInner2 = pal(minDist2 * 2.0 + time * 0.1,
            float3(0.5, 0.5, 0.5), float3(0.5, 0.5, 0.5),
            float3(1.0, 0.7, 0.4), float3(0.00, 0.33, 0.67));
        col2 = trapInner2 * trapGlow2 * 0.4
            + axGlow2 * float3(0.1, 0.05, 0.25) * (0.3 + bass * 0.4);
        col2 *= 0.3 + energy * 0.7;
    }

    // --- Crossfade with a bright flash at the transition ---
    float flash = exp(-pow((fadeT - 0.5) * 4.0, 2.0)) * 0.8;
    float3 flashCol = float3(0.6 + bass * 0.4, 0.5 + mid * 0.3, 1.0);
    col = lerp(col, col2, fadeT) + flash * flashCol;

    col *= 0.7 + bass * 0.4 + energy * 0.3;

    // Vignette
    float2 vc = uv - 0.5;
    float vignette = 1.0 - dot(vc, vc) * 1.2;
    col *= smoothstep(0.0, 1.0, vignette);

    return float4(col, 1.0);
}
)";

// ============================================================
// Shader: Warp Tunnel
// ============================================================
static const char* g_psTunnel = R"(
cbuffer CB : register(b0) {
    float2 resolution;
    float time;
    float bass;
    float mid;
    float treble;
    float energy;
    float pad;
    float bands[32];
};

float3 palette(float t) {
    return 0.5 + 0.5 * cos(6.28318 * (t + float3(0.0, 0.1, 0.2)));
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 p = (uv - 0.5) * float2(resolution.x / resolution.y, 1.0);

    float angle = atan2(p.y, p.x);
    float radius = length(p);

    // Tunnel coordinates — use sin/cos of angle multiples to avoid atan2 seam
    float tunnelZ = 1.0 / (radius + 0.001) + time * (0.5 + bass * 2.0);

    // Ring pattern with frequency response
    float pattern = 0.0;
    for (int i = 0; i < 8; i++) {
        float freq = bands[i * 2] * 2.0;
        float n = (float)(3 + i);
        // Use sin(n*angle) directly — continuous everywhere, no seam
        float angTerm = sin(n * angle + time * 0.1 * (1.0 + mid));
        float ring = sin(tunnelZ * (2.0 + i * 0.5) + angTerm * 2.0) * 0.5 + 0.5;
        pattern += ring * (0.1 + freq * 0.15);
    }

    float3 col = palette(pattern + time * 0.05 + treble);

    // Brightness based on depth and energy
    float depth = exp(-radius * 2.0);
    col *= depth * (0.6 + energy * 1.5);

    // Pulse rings from bass
    float ring = abs(sin(radius * 20.0 - time * 4.0 * (1.0 + bass)));
    ring = smoothstep(0.95, 1.0, ring);
    col += ring * float3(0.3, 0.5, 1.0) * bass;

    // Center glow
    float glow = exp(-radius * 8.0) * energy * 2.0;
    col += glow * float3(1.0, 0.7, 0.4);

    return float4(col, 1.0);
}
)";

// ============================================================
// Shader: Plasma Wave
// ============================================================
static const char* g_psPlasma = R"(
cbuffer CB : register(b0) {
    float2 resolution;
    float time;
    float bass;
    float mid;
    float treble;
    float energy;
    float pad;
    float bands[32];
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 p = (uv - 0.5) * float2(resolution.x / resolution.y, 1.0) * 3.0;

    float v = 0.0;

    // Layered sine waves driven by frequency bands
    v += sin(p.x * (2.0 + bands[0] * 4.0) + time * 1.5);
    v += sin((p.y * (2.0 + bands[2] * 4.0) + time) * 0.7);
    v += sin((p.x + p.y) * (1.5 + bands[4] * 3.0) + time * 0.6);

    float cx = p.x + 0.5 * sin(time * 0.3 + mid);
    float cy = p.y + 0.5 * cos(time * 0.4 + bass);
    v += sin(sqrt(cx * cx + cy * cy + 1.0) * (3.0 + bands[6] * 5.0) - time * 2.0);

    v *= 0.5;

    // Color channels offset by different frequency bands
    float3 col;
    col.r = sin(v * 3.14159 + time + treble * 6.0) * 0.5 + 0.5;
    col.g = sin(v * 3.14159 + time * 1.1 + 2.094 + mid * 4.0) * 0.5 + 0.5;
    col.b = sin(v * 3.14159 + time * 0.9 + 4.189 + bass * 3.0) * 0.5 + 0.5;

    // Ripples from bass hits
    float dist = length(uv - 0.5);
    float ripple = sin(dist * 30.0 - time * 8.0) * exp(-dist * 4.0) * bass;
    col += ripple * 0.4;

    col *= 0.5 + energy * 1.0;

    return float4(col, 1.0);
}
)";

// ============================================================

bool ShaderVis::CompileShader(const char* hlsl, const char* entry, ID3D11PixelShader** ps) {
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr,
        entry, "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            printf("[Shader] Compile error: %s\n", (char*)errors->GetBufferPointer());
            errors->Release();
        }
        return false;
    }
    if (errors) errors->Release();

    hr = m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, ps);
    blob->Release();
    return SUCCEEDED(hr);
}

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

    // Compile pixel shaders
    m_shaderCount = 0;

    if (CompileShader(g_psFractal, "main", &m_pixelShaders[m_shaderCount])) {
        m_shaderNames[m_shaderCount] = "Fractal Dreamscape";
        m_shaderCount++;
    }
    if (CompileShader(g_psTunnel, "main", &m_pixelShaders[m_shaderCount])) {
        m_shaderNames[m_shaderCount] = "Warp Tunnel";
        m_shaderCount++;
    }
    if (CompileShader(g_psPlasma, "main", &m_pixelShaders[m_shaderCount])) {
        m_shaderNames[m_shaderCount] = "Plasma Wave";
        m_shaderCount++;
    }

    if (m_shaderCount == 0) return false;

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
    if (!m_outputRTV || m_currentShader >= m_shaderCount) return;

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
    m_ctx->PSSetShader(m_pixelShaders[m_currentShader], nullptr, 0);
    m_ctx->PSSetConstantBuffers(0, 1, &m_cbuffer);
    m_ctx->Draw(3, 0);

    // Restore state
    m_ctx->OMSetRenderTargets(1, &prevRTV, prevDSV);
    m_ctx->RSSetViewports(1, &prevVP);
    if (prevRTV) prevRTV->Release();
    if (prevDSV) prevDSV->Release();
}

void ShaderVis::SetShader(int index) {
    if (index >= 0 && index < m_shaderCount)
        m_currentShader = index;
}

const char* ShaderVis::GetShaderName(int index) const {
    if (index >= 0 && index < m_shaderCount)
        return m_shaderNames[index];
    return "Unknown";
}

void ShaderVis::Cleanup() {
    if (m_outputSRV) { m_outputSRV->Release(); m_outputSRV = nullptr; }
    if (m_outputRTV) { m_outputRTV->Release(); m_outputRTV = nullptr; }
    if (m_outputTex) { m_outputTex->Release(); m_outputTex = nullptr; }
    if (m_cbuffer) { m_cbuffer->Release(); m_cbuffer = nullptr; }
    if (m_rasterState) { m_rasterState->Release(); m_rasterState = nullptr; }
    if (m_vsFullscreen) { m_vsFullscreen->Release(); m_vsFullscreen = nullptr; }
    for (int i = 0; i < m_shaderCount; i++) {
        if (m_pixelShaders[i]) { m_pixelShaders[i]->Release(); m_pixelShaders[i] = nullptr; }
    }
    m_shaderCount = 0;
}
