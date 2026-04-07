# HVis

Real-time audio visualiser for Windows. Captures system audio via WASAPI loopback and drives GPU shader visualisations and post-processing effects.

![Stack](https://img.shields.io/badge/C%2B%2B17-D3D11%20%2B%20ImGui-blue)
![Platform](https://img.shields.io/badge/platform-Windows%2010%2B-lightgrey)

## Features

### Audio Engine
- **WASAPI loopback capture** — visualises whatever your system is playing, no configuration needed
- Silence-render trick keeps the audio device active even when nothing is playing
- Real-time FFT with Hann windowing, smooth spectrum, and frequency band analysis
- Audio decomposed into bass / mid / treble energy + 32 log-spaced frequency bands

### Visualisations

**Waveform / Spectrum** — Classic amplitude waveform and logarithmic frequency spectrum with amplitude-based coloring.

**Fractal Dreamscape** — Infinite-zoom Mandelbrot explorer targeting Misiurewicz points (mathematically guaranteed boundary points). Cycles through 5 targets with seamless crossfade transitions. Audio drives rotation, orbit trap animation, palette cycling, and color blending.

**Warp Tunnel** — Polar-coordinate tunnel with layered ring patterns. Each ring layer responds to a different frequency band. Bass creates radial pulse rings, center glows with energy.

**Plasma Wave** — Layered sine interference patterns modulated by frequency bands. Bass-driven ripples radiate from center. Color channels offset by different audio parameters.

### Post-Processing

All effects are audio-reactive and individually toggleable:

| Effect | Audio Link |
|--------|-----------|
| **Bloom** | Bass pulses glow intensity |
| **Chromatic Aberration** | Treble drives RGB split |
| **Feedback Trails** | Ghosting/echo from previous frames |
| **Radial Blur** | Bass punches zoom blur from center |
| **Film Grain** | Energy controls noise amount |
| **Scanlines** | CRT-style horizontal lines |
| **Vignette** | Edge darkening |
| **Color Grading** | Temperature, contrast, saturation |

Settings are saved to `postfx.cfg` and persist across sessions.

## Architecture

```
WASAPI Loopback ─► AudioCapture ─► FFT + Band Analysis ─► AudioParams
                                                              │
                          ┌───────────────────────────────────┘
                          ▼
                     ShaderVis ─► Pixel Shader (offscreen texture)
                          │
                          ▼
                    PostProcess ─► Post-FX Shader (offscreen texture)
                          │
                          ▼
                   ImGui::Image ─► D3D11 Swap Chain ─► Display
```

- **Render thread** — all D3D11 and ImGui work runs on a dedicated thread, decoupled from the Win32 message pump. The window stays responsive during drag/resize.
- **Message queue** — Win32 input messages are queued from the main thread and replayed on the render thread, keeping ImGui thread-safe.
- **Shader pipeline** — each visualisation is a self-contained HLSL pixel shader compiled at startup. Shaders render to an offscreen texture via fullscreen triangle (no vertex buffer). Post-processing is a separate pass reading that texture.

## Building

### Requirements

- **Visual Studio 2022** with C++ desktop workload (MSVC v143, C++17)
- **ImGui 1.92.3** at `C:\dev\imgui-1.92.3` (source files compiled directly into the project)
- **Windows 10 SDK**

### Build

Open `HVis.sln` in Visual Studio and build (F5), or from command line:

```
msbuild HVis.sln /p:Configuration=Debug /p:Platform=x64
```

Output: `x64\Debug\HVis.exe`

No external package manager or dependencies beyond ImGui and the Windows SDK.

## Project Structure

```
HVis/
  HVis.sln
  HVis/
    main.cpp             App entry, D3D11 setup, render thread, ImGui UI
    AudioCapture.h/.cpp  WASAPI loopback capture
    ShaderVis.h/.cpp     Shader compilation, visualisation rendering
    PostProcess.h/.cpp   Post-processing effects pipeline
    HVis.vcxproj
```

## Usage

Run the exe. Play audio on your system. Switch between visualisations using the buttons in the header bar. Click **PostFX** to open the post-processing settings panel.

## License

MIT
