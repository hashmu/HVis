# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

```bash
# Debug build
msbuild HVis.sln /p:Configuration=Debug /p:Platform=x64

# Release build
msbuild HVis.sln /p:Configuration=Release /p:Platform=x64

# Clean + rebuild (clears stale intermediates)
msbuild HVis.sln /t:Clean;Build /p:Configuration=Debug /p:Platform=x64
```

Output: `x64/Debug/HVis.exe` or `x64/Release/HVis.exe`

No tests, no linter, no package manager. If VS isn't on PATH, the full msbuild path is:
```
"/c/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe"
```

## Dependencies

- **ImGui 1.92.3** compiled from source at `C:\dev\imgui-1.92.3` (hardcoded in vcxproj include paths)
- **D3D11, DXGI, D3DCompiler, WASAPI** from Windows SDK
- No vcpkg, no submodules, no external libs

## Architecture

### Threading

Two threads with strict ownership:

- **Main thread**: Win32 message pump only (`GetMessage` loop). Queues input messages into a lock-protected ring buffer (`g_msgQueue`). Sets resize flags via `g_resizeCS`.
- **Render thread**: Owns all D3D11 and ImGui state. Drains message queue into `ImGui_ImplWin32_WndProcHandler`, handles deferred resize, polls audio, renders frame. Runs continuously with VSync (`Present(1,0)`).

Critical sections (`g_resizeCS`, `g_msgCS`) must be initialized before `CreateWindowW` because `WndProc` fires during window creation.

### Render Pipeline

```
AudioCapture → PollAudio() → AudioParams (bass/mid/treble/energy + 32 bands)
                                    ↓
ShaderVis::Render() → offscreen texture (fullscreen triangle, no VB)
                                    ↓
PostProcess::Apply() → reads shader output SRV → post-FX texture
                                    ↓
ImGui::Image() → D3D11 back buffer → Present
```

All shaders use the **fullscreen triangle trick**: 3 vertices from `SV_VertexID`, no vertex buffer or input layout needed.

### Shader System (ShaderVis)

HLSL source is embedded as raw C string literals in `ShaderVis.cpp`. Compiled at startup via `D3DCompile`. Each visualisation is a standalone pixel shader sharing a common constant buffer layout:

```hlsl
cbuffer CB : register(b0) {
    float2 resolution; float time;
    float bass, mid, treble, energy, pad;
    float bands[32];
};
```

**Adding a new shader**: write the HLSL string, add a `CompileShader()` + `m_shaderNames[]` entry in `ShaderVis::Init()`.

HLSL pitfalls encountered in this project:
- `static const float2 array[N]` works for array initialization (despite common belief otherwise)
- `atan2` in tight iteration loops can cause GPU timeouts or TDR — keep trig out of Mandelbrot inner loops
- float32 precision limits Mandelbrot zoom to ~10^4x magnification (`exp(-9)`)

### Post-Processing (PostProcess)

Separate fullscreen pass reading the shader output as `t0` SRV and previous frame as `t1` SRV (for feedback trails). Toggle/intensity for each effect passed via constant buffer as floats (0.0/1.0 for bools).

Settings persisted as binary POD struct to `postfx.cfg` with magic number + version guard.

### Audio (AudioCapture)

WASAPI loopback in shared mode. Uses a **dual-client pattern**: one capture client reads system audio, one render client continuously feeds silence to prevent the loopback device from going idle when no app is playing audio.

Audio analysis in `PollAudio()`:
- Downmix to mono, feed ring buffer
- DFT (not FFT — simple O(n²), sufficient for 1024 display bins)
- Hann window, smooth magnitude with 0.7/0.3 exponential decay
- Band decomposition: bass <300Hz, mid 300-2kHz, treble >2kHz
- 32 log-spaced bands via quadratic `t²` distribution

## Key Files

| File | Role |
|------|------|
| `HVis/main.cpp` | Entry point, D3D11 setup, render thread, ImGui UI, audio polling, waveform/spectrum drawing |
| `HVis/ShaderVis.cpp` | 3 embedded HLSL shaders (Fractal, Tunnel, Plasma), shader compilation, offscreen render |
| `HVis/PostProcess.cpp` | Post-FX HLSL shader, 8 effects, settings save/load |
| `HVis/AudioCapture.cpp` | WASAPI loopback, silence-render trick, audio data retrieval |
