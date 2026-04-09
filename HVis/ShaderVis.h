#pragma once
#include <d3d11.h>
#include <d3dcompiler.h>
#include <string>
#include <vector>

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

    int GetShaderCount() const { return (int)m_shaders.size(); }
    void SetShader(int index);
    int GetCurrentShader() const { return m_currentShader; }
    const char* GetShaderName(int index) const;

private:
    struct ShaderEntry {
        ID3D11PixelShader* ps = nullptr;
        std::string name;       // display name (filename without .hlsl)
        std::string path;       // full file path
        FILETIME lastWrite;     // for hot reload
    };

    bool CompilePixelShader(const char* hlsl, const char* name, ID3D11PixelShader** ps);
    void ScanDirectory();
    void CheckHotReload();
    bool LoadShaderFile(const std::string& path, ShaderEntry& entry);
    void CreateOutputTexture(UINT width, UINT height);

    static std::string FindShadersDir();
    static std::string ReadTextFile(const std::string& path);
    static FILETIME GetFileWriteTime(const std::string& path);

    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_ctx = nullptr;

    // Fullscreen triangle VS
    ID3D11VertexShader* m_vsFullscreen = nullptr;

    // Pixel shaders (loaded from files)
    std::vector<ShaderEntry> m_shaders;
    int m_currentShader = 0;

    // Shader directory
    std::string m_shaderDir;

    // Hot reload
    int m_reloadCounter = 0;
    static const int RELOAD_INTERVAL = 30;

    // Constant buffer
    ID3D11Buffer* m_cbuffer = nullptr;

    // Output render target
    ID3D11Texture2D* m_outputTex = nullptr;
    ID3D11RenderTargetView* m_outputRTV = nullptr;
    ID3D11ShaderResourceView* m_outputSRV = nullptr;
    UINT m_width = 0;
    UINT m_height = 0;

    // Rasterizer state
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
