#pragma once
#include <d3d11.h>
#include <d3dcompiler.h>

struct PostProcessSettings {
    void Save(const char* path) const;
    void Load(const char* path);

    // Bloom
    bool bloom = true;
    float bloomIntensity = 0.5f;
    float bloomThreshold = 0.6f;
    bool bloomAudioReactive = true;

    // Chromatic aberration
    bool chromaticAberration = true;
    float caIntensity = 0.003f;
    bool caAudioReactive = true;

    // Film grain
    bool filmGrain = false;
    float grainIntensity = 0.08f;
    bool grainAudioReactive = true;

    // Feedback trails
    bool feedback = false;
    float feedbackAmount = 0.85f;
    bool feedbackAudioReactive = true;

    // Radial blur
    bool radialBlur = false;
    float radialBlurIntensity = 0.02f;
    bool radialBlurAudioReactive = true;

    // Scanlines
    bool scanlines = false;
    float scanlineIntensity = 0.15f;

    // Vignette (unified)
    bool vignette = true;
    float vignetteIntensity = 1.2f;

    // Color grading
    bool colorGrading = false;
    float temperature = 0.0f;  // -1 cool, +1 warm
    float contrast = 1.0f;
    float saturation = 1.0f;
};

class PostProcess {
public:
    bool Init(ID3D11Device* device, ID3D11DeviceContext* ctx);
    void Resize(UINT width, UINT height);
    void Apply(ID3D11ShaderResourceView* inputSRV, float time,
               float bass, float mid, float treble, float energy,
               const PostProcessSettings& settings);
    void Cleanup();

    ID3D11ShaderResourceView* GetOutputSRV() const { return m_outputSRV; }

private:
    void CreateTextures(UINT width, UINT height);

    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_ctx = nullptr;

    ID3D11VertexShader* m_vsFullscreen = nullptr;
    ID3D11PixelShader* m_psPostProcess = nullptr;

    // Constant buffer
    ID3D11Buffer* m_cbuffer = nullptr;

    // Output
    ID3D11Texture2D* m_outputTex = nullptr;
    ID3D11RenderTargetView* m_outputRTV = nullptr;
    ID3D11ShaderResourceView* m_outputSRV = nullptr;

    // Feedback (previous frame)
    ID3D11Texture2D* m_feedbackTex = nullptr;
    ID3D11ShaderResourceView* m_feedbackSRV = nullptr;
    ID3D11RenderTargetView* m_feedbackRTV = nullptr;

    // Sampler
    ID3D11SamplerState* m_sampler = nullptr;
    ID3D11RasterizerState* m_rasterState = nullptr;

    UINT m_width = 0;
    UINT m_height = 0;

    struct alignas(16) PostCB {
        float resolution[2];
        float time;
        float bass;
        float mid;
        float treble;
        float energy;
        // Effect toggles and params (packed as floats for HLSL)
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
        float pad[3];
    };
};
