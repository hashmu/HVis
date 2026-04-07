#include "PostProcess.h"
#include <stdio.h>
#include <string.h>

static const unsigned int SETTINGS_MAGIC = 0x48565050; // "HVPP"
static const unsigned int SETTINGS_VERSION = 1;

void PostProcessSettings::Save(const char* path) const {
    FILE* f = nullptr;
    fopen_s(&f, path, "wb");
    if (!f) return;
    fwrite(&SETTINGS_MAGIC, 4, 1, f);
    fwrite(&SETTINGS_VERSION, 4, 1, f);
    fwrite(this, sizeof(PostProcessSettings), 1, f);
    fclose(f);
}

void PostProcessSettings::Load(const char* path) {
    FILE* f = nullptr;
    fopen_s(&f, path, "rb");
    if (!f) return;
    unsigned int magic = 0, version = 0;
    fread(&magic, 4, 1, f);
    fread(&version, 4, 1, f);
    if (magic == SETTINGS_MAGIC && version == SETTINGS_VERSION) {
        fread(this, sizeof(PostProcessSettings), 1, f);
    }
    fclose(f);
}

static const char* g_vsFullscreen = R"(
void main(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    uv.y = 1.0 - uv.y;
}
)";

static const char* g_psPostProcess = R"(
Texture2D texInput : register(t0);
Texture2D texFeedback : register(t1);
SamplerState samp : register(s0);

cbuffer CB : register(b0) {
    float2 resolution;
    float time;
    float bass;
    float mid;
    float treble;
    float energy;
    float bloomOn;
    float bloomIntensity;
    float bloomThreshold;
    float caOn;
    float caIntensity;
    float grainOn;
    float grainIntensity;
    float feedbackOn;
    float feedbackAmount;
    float radialBlurOn;
    float radialBlurIntensity;
    float scanlinesOn;
    float scanlineIntensity;
    float vignetteOn;
    float vignetteIntensity;
    float colorGradingOn;
    float temperature;
    float contrast;
    float saturation;
    float pad0, pad1, pad2;
};

// Hash for grain
float hash(float2 p) {
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float3 sampleInput(float2 uv) {
    return texInput.Sample(samp, uv).rgb;
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float3 col;

    // --- Chromatic aberration ---
    if (caOn > 0.5) {
        float ca = caIntensity * (1.0 + treble * 2.0);
        float2 dir = (uv - 0.5) * ca;
        col.r = sampleInput(uv + dir).r;
        col.g = sampleInput(uv).g;
        col.b = sampleInput(uv - dir).b;
    } else {
        col = sampleInput(uv);
    }

    // --- Radial blur ---
    if (radialBlurOn > 0.5) {
        float blur = radialBlurIntensity * (1.0 + bass * 3.0);
        float2 dir = (uv - 0.5) * blur;
        float3 sum = col;
        for (int i = 1; i <= 8; i++) {
            float t = (float)i / 8.0;
            sum += sampleInput(uv - dir * t);
        }
        col = sum / 9.0;
    }

    // --- Bloom (simple 9-tap blur of bright areas) ---
    if (bloomOn > 0.5) {
        float bi = bloomIntensity * (1.0 + bass * 1.5);
        float2 texel = 1.0 / resolution;
        float3 bloom = float3(0, 0, 0);
        float count = 0;
        for (int bx = -2; bx <= 2; bx++) {
            for (int by = -2; by <= 2; by++) {
                float3 s = sampleInput(uv + float2(bx, by) * texel * 3.0);
                float lum = dot(s, float3(0.299, 0.587, 0.114));
                if (lum > bloomThreshold) {
                    bloom += s;
                    count += 1.0;
                }
            }
        }
        if (count > 0) bloom /= count;
        col += bloom * bi;
    }

    // --- Feedback trails ---
    if (feedbackOn > 0.5) {
        float fb = feedbackAmount;
        float3 prev = texFeedback.Sample(samp, uv).rgb;
        col = max(col, prev * fb);
    }

    // --- Film grain ---
    if (grainOn > 0.5) {
        float gi = grainIntensity * (0.5 + energy * 1.0);
        float grain = hash(uv * resolution + time * 1000.0) - 0.5;
        col += grain * gi;
    }

    // --- Scanlines ---
    if (scanlinesOn > 0.5) {
        float scanline = sin(uv.y * resolution.y * 3.14159) * 0.5 + 0.5;
        scanline = lerp(1.0, scanline, scanlineIntensity);
        col *= scanline;
    }

    // --- Vignette ---
    if (vignetteOn > 0.5) {
        float2 vc = uv - 0.5;
        float vig = 1.0 - dot(vc, vc) * vignetteIntensity;
        col *= smoothstep(0.0, 1.0, vig);
    }

    // --- Color grading ---
    if (colorGradingOn > 0.5) {
        // Temperature shift
        col.r += temperature * 0.1;
        col.b -= temperature * 0.1;

        // Contrast
        col = (col - 0.5) * contrast + 0.5;

        // Saturation
        float grey = dot(col, float3(0.299, 0.587, 0.114));
        col = lerp(float3(grey, grey, grey), col, saturation);
    }

    col = saturate(col);
    return float4(col, 1.0);
}
)";

bool PostProcess::Init(ID3D11Device* device, ID3D11DeviceContext* ctx) {
    m_device = device;
    m_ctx = ctx;

    // Vertex shader
    {
        ID3DBlob* blob = nullptr;
        ID3DBlob* errors = nullptr;
        HRESULT hr = D3DCompile(g_vsFullscreen, strlen(g_vsFullscreen), nullptr, nullptr, nullptr,
            "main", "vs_5_0", 0, 0, &blob, &errors);
        if (FAILED(hr)) {
            if (errors) { printf("[PostFX] VS error: %s\n", (char*)errors->GetBufferPointer()); errors->Release(); }
            return false;
        }
        if (errors) errors->Release();
        hr = m_device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_vsFullscreen);
        blob->Release();
        if (FAILED(hr)) return false;
    }

    // Pixel shader
    {
        ID3DBlob* blob = nullptr;
        ID3DBlob* errors = nullptr;
        HRESULT hr = D3DCompile(g_psPostProcess, strlen(g_psPostProcess), nullptr, nullptr, nullptr,
            "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
        if (FAILED(hr)) {
            if (errors) { printf("[PostFX] PS error: %s\n", (char*)errors->GetBufferPointer()); errors->Release(); }
            return false;
        }
        if (errors) errors->Release();
        hr = m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_psPostProcess);
        blob->Release();
        if (FAILED(hr)) return false;
    }

    // Constant buffer
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(PostCB);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_device->CreateBuffer(&bd, nullptr, &m_cbuffer))) return false;

    // Sampler
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    m_device->CreateSamplerState(&sd, &m_sampler);

    // Rasterizer
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    m_device->CreateRasterizerState(&rd, &m_rasterState);

    return true;
}

void PostProcess::CreateTextures(UINT width, UINT height) {
    // Release old
    if (m_outputSRV) { m_outputSRV->Release(); m_outputSRV = nullptr; }
    if (m_outputRTV) { m_outputRTV->Release(); m_outputRTV = nullptr; }
    if (m_outputTex) { m_outputTex->Release(); m_outputTex = nullptr; }
    if (m_feedbackSRV) { m_feedbackSRV->Release(); m_feedbackSRV = nullptr; }
    if (m_feedbackRTV) { m_feedbackRTV->Release(); m_feedbackRTV = nullptr; }
    if (m_feedbackTex) { m_feedbackTex->Release(); m_feedbackTex = nullptr; }

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

    m_device->CreateTexture2D(&td, nullptr, &m_feedbackTex);
    m_device->CreateRenderTargetView(m_feedbackTex, nullptr, &m_feedbackRTV);
    m_device->CreateShaderResourceView(m_feedbackTex, nullptr, &m_feedbackSRV);

    m_width = width;
    m_height = height;
}

void PostProcess::Resize(UINT width, UINT height) {
    if (width == 0 || height == 0) return;
    if (width != m_width || height != m_height)
        CreateTextures(width, height);
}

void PostProcess::Apply(ID3D11ShaderResourceView* inputSRV, float time,
                        float bass, float mid, float treble, float energy,
                        const PostProcessSettings& s) {
    if (!m_outputRTV || !inputSRV) return;

    // Update constant buffer
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m_ctx->Map(m_cbuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        PostCB* cb = (PostCB*)mapped.pData;
        cb->resolution[0] = (float)m_width;
        cb->resolution[1] = (float)m_height;
        cb->time = time;
        cb->bass = bass;
        cb->mid = mid;
        cb->treble = treble;
        cb->energy = energy;
        cb->bloomOn = s.bloom ? 1.0f : 0.0f;
        cb->bloomIntensity = s.bloomIntensity * (s.bloomAudioReactive ? 1.0f : 1.0f);
        cb->bloomThreshold = s.bloomThreshold;
        cb->caOn = s.chromaticAberration ? 1.0f : 0.0f;
        cb->caIntensity = s.caIntensity;
        cb->grainOn = s.filmGrain ? 1.0f : 0.0f;
        cb->grainIntensity = s.grainIntensity;
        cb->feedbackOn = s.feedback ? 1.0f : 0.0f;
        cb->feedbackAmount = s.feedbackAmount;
        cb->radialBlurOn = s.radialBlur ? 1.0f : 0.0f;
        cb->radialBlurIntensity = s.radialBlurIntensity;
        cb->scanlinesOn = s.scanlines ? 1.0f : 0.0f;
        cb->scanlineIntensity = s.scanlineIntensity;
        cb->vignetteOn = s.vignette ? 1.0f : 0.0f;
        cb->vignetteIntensity = s.vignetteIntensity;
        cb->colorGradingOn = s.colorGrading ? 1.0f : 0.0f;
        cb->temperature = s.temperature;
        cb->contrast = s.contrast;
        cb->saturation = s.saturation;
        memset(cb->pad, 0, sizeof(cb->pad));
        m_ctx->Unmap(m_cbuffer, 0);
    }

    // Save state
    ID3D11RenderTargetView* prevRTV = nullptr;
    ID3D11DepthStencilView* prevDSV = nullptr;
    D3D11_VIEWPORT prevVP;
    UINT numVP = 1;
    m_ctx->OMGetRenderTargets(1, &prevRTV, &prevDSV);
    m_ctx->RSGetViewports(&numVP, &prevVP);

    // Render post-process to output texture
    m_ctx->OMSetRenderTargets(1, &m_outputRTV, nullptr);
    D3D11_VIEWPORT vp = {};
    vp.Width = (float)m_width;
    vp.Height = (float)m_height;
    vp.MaxDepth = 1.0f;
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->RSSetState(m_rasterState);

    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->IASetInputLayout(nullptr);
    m_ctx->VSSetShader(m_vsFullscreen, nullptr, 0);
    m_ctx->PSSetShader(m_psPostProcess, nullptr, 0);
    m_ctx->PSSetConstantBuffers(0, 1, &m_cbuffer);

    ID3D11ShaderResourceView* srvs[2] = { inputSRV, m_feedbackSRV };
    m_ctx->PSSetShaderResources(0, 2, srvs);
    m_ctx->PSSetSamplers(0, 1, &m_sampler);

    m_ctx->Draw(3, 0);

    // Unbind SRVs to allow copy
    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
    m_ctx->PSSetShaderResources(0, 2, nullSRVs);

    // Copy output to feedback for next frame
    if (s.feedback)
        m_ctx->CopyResource(m_feedbackTex, m_outputTex);

    // Restore state
    m_ctx->OMSetRenderTargets(1, &prevRTV, prevDSV);
    m_ctx->RSSetViewports(1, &prevVP);
    if (prevRTV) prevRTV->Release();
    if (prevDSV) prevDSV->Release();
}

void PostProcess::Cleanup() {
    if (m_outputSRV) { m_outputSRV->Release(); m_outputSRV = nullptr; }
    if (m_outputRTV) { m_outputRTV->Release(); m_outputRTV = nullptr; }
    if (m_outputTex) { m_outputTex->Release(); m_outputTex = nullptr; }
    if (m_feedbackSRV) { m_feedbackSRV->Release(); m_feedbackSRV = nullptr; }
    if (m_feedbackRTV) { m_feedbackRTV->Release(); m_feedbackRTV = nullptr; }
    if (m_feedbackTex) { m_feedbackTex->Release(); m_feedbackTex = nullptr; }
    if (m_cbuffer) { m_cbuffer->Release(); m_cbuffer = nullptr; }
    if (m_sampler) { m_sampler->Release(); m_sampler = nullptr; }
    if (m_rasterState) { m_rasterState->Release(); m_rasterState = nullptr; }
    if (m_psPostProcess) { m_psPostProcess->Release(); m_psPostProcess = nullptr; }
    if (m_vsFullscreen) { m_vsFullscreen->Release(); m_vsFullscreen = nullptr; }
}
