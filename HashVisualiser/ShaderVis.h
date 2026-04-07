#pragma once
#include <d3d11.h>
#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

struct AudioParams {
    float bass;
    float mid;
    float treble;
    float energy;
    float bands[32];
};

class ShaderVis {
public:
    bool Init(ID3D11Device* device, ID3D11DeviceContext* ctx);
    void Resize(UINT width, UINT height);
    void Update(float time, const AudioParams& audio);
    void Render();
    void Cleanup();

    ID3D11ShaderResourceView* GetOutputSRV() const { return m_outputSRV; }

    int GetShaderCount() const { return m_shaderCount; }
    void SetShader(int index);
    int GetCurrentShader() const { return m_currentShader; }
    const char* GetShaderName(int index) const;

private:
    bool CompileShader(const char* hlsl, const char* entry, ID3D11PixelShader** ps);
    void CreateOutputTexture(UINT width, UINT height);

    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_ctx = nullptr;

    // Fullscreen triangle
    ID3D11VertexShader* m_vsFullscreen = nullptr;

    // Pixel shaders
    static const int MAX_SHADERS = 8;
    ID3D11PixelShader* m_pixelShaders[MAX_SHADERS] = {};
    const char* m_shaderNames[MAX_SHADERS] = {};
    int m_shaderCount = 0;
    int m_currentShader = 0;

    // Constant buffer
    ID3D11Buffer* m_cbuffer = nullptr;

    // Output render target
    ID3D11Texture2D* m_outputTex = nullptr;
    ID3D11RenderTargetView* m_outputRTV = nullptr;
    ID3D11ShaderResourceView* m_outputSRV = nullptr;
    UINT m_width = 0;
    UINT m_height = 0;

    // Rasterizer/sampler state
    ID3D11RasterizerState* m_rasterState = nullptr;

    // CB layout (must match HLSL)
    struct alignas(16) ShaderCB {
        float resolution[2];
        float time;
        float bass;
        float mid;
        float treble;
        float energy;
        float pad;
        float bands[32];
    };
};
