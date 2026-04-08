# Water Rendering: Techniques & References

## Sea of Thieves — Technical Approach

Primary source: **Rob Sherlock**, "The Technical Art of Sea of Thieves" (GDC 2018).

### Wave Simulation

Sea of Thieves uses **Gerstner waves** rather than FFT-based ocean simulation. This was a deliberate choice for performance on Xbox One and for artistic controllability.

- Multiple Gerstner wave octaves summed with different frequencies, amplitudes, and directions
- Wave parameters driven by a wind system controlling amplitude, frequency, and choppiness
- Vertex displacement in the **vertex shader** — the ocean is a mesh with displaced vertices, not a raymarched height field

A single Gerstner wave displaces a surface point (x, z) as:

```
X' = x - Q * A * Dx * sin(dot(D, xz) * w + phi * t)
Z' = z - Q * A * Dz * sin(dot(D, xz) * w + phi * t)
Y  = A * cos(dot(D, xz) * w + phi * t)
```

Where:
- **D** = wave direction (unit vector)
- **w** = angular frequency
- **A** = amplitude
- **Q** = steepness parameter (0 = sinusoidal, 1 = cusps/breaking)
- **phi** = phase speed

Gerstner waves produce realistic peaked crests and flat troughs, unlike simple sine waves. The horizontal displacement (X', Z') causes surface points to bunch at crests, which is what creates the sharp peak shape.

### Mesh & LOD

The ocean uses a **projected grid** (screen-space grid projected onto the water plane):
- Dense triangles near camera, sparse at distance — automatic LOD
- No tile-based LOD seams
- Vertex count naturally limited to what's visible

Reference: Mark Finch, "Effective Water Simulation from Physical Models" (GPU Gems Chapter 1).

### Shading

- **Subsurface scattering (SSS)**: Approximated by checking wave thickness relative to light direction. Thin wave crests transmit light, giving the characteristic teal glow
- **Fresnel**: Schlick approximation — steep viewing angles reflect more, shallow angles show water depth
- **Foam**: Driven by the **Gerstner Jacobian** — the determinant of the horizontal displacement map. Where surface points converge (Jacobian < 1), waves are compressing and foam appears. Layered with scrolling noise textures
- **Reflection**: Cubemap + screen-space reflections (SSR)
- **Refraction**: Distorted grab-pass of the underwater scene

### The Jacobian for Foam

The Jacobian of Gerstner displacement measures surface convergence/divergence:

```
Jxx = 1 - sum(Q[i] * Dx[i]^2 * w[i] * A[i] * sin(phase[i]))
Jzz = 1 - sum(Q[i] * Dz[i]^2 * w[i] * A[i] * sin(phase[i]))
Jxz =   - sum(Q[i] * Dx[i] * Dz[i] * w[i] * A[i] * sin(phase[i]))
J = Jxx * Jzz - Jxz^2
```

When J drops below 1, the surface is converging (foam). When J drops below 0, the surface would self-intersect (wave breaking). The Q parameter must be clamped so `Q * w * A < 1` to prevent self-intersection artifacts.

## HVis Ocean Shaders

### Ocean (original) — Psychedelic FBM

Raymarched height field using 5-octave FBM noise with hash-based value noise. Visually psychedelic with palette-cycled colors and aurora effects.

**Performance characteristics:**
- 48 raymarch steps + 6 binary refinement steps
- Each height evaluation: 5 FBM octaves (each with 4 hash lookups + lerps) + 3 sine swells + 4 band-reactive ripples ≈ 23 operations
- Normals via finite differences (3x height evaluations)
- Total per pixel: ~57 height evaluations × ~23 ops ≈ 1300 operations
- Too expensive for 4K even at half resolution on RTX Ada 4000

### Ocean (Gerstner) — Realistic

Raymarched height field using summed Gerstner waves. Adapted from Sea of Thieves' approach but constrained to a pixel shader (no mesh/vertex displacement).

**Wave setup:**
- 6 Gerstner waves with geometrically spaced frequencies (0.18 to 2.0)
- Amplitudes decrease with frequency (1.2 down to 0.04)
- Directions spread across ~95-degree arc for natural interference
- Steepness (Q) per wave controls crest sharpness

**Performance characteristics:**
- 24 raymarch steps + 4 binary refinement steps
- Each height evaluation: 6 cos operations ≈ 6 ops
- **Analytical normals** from closed-form Gerstner derivatives (6 sin ops, computed once)
- **Analytical foam** from Jacobian (reuses sin values from normal computation)
- Far distance LOD: rays hitting water beyond t=300 skip raymarch entirely
- Total per pixel: ~28 height evaluations × 6 ops + 12 (normal+foam) ≈ 180 ops
- Roughly **7-8x cheaper** than the FBM ocean

**Audio mapping:**
- Bass → primary swell amplitude
- Mid → wave steepness (Q parameter, choppiness)
- Treble → foam visibility
- Energy → overall brightness
- Bands[0..5] → individual wave amplitudes (low audio freq → slow swells, high → choppy detail)

**Shading:**
- Deep blue-green water color (realistic, not palette-cycled)
- Schlick Fresnel (F0=0.02 for water IOR ~1.33)
- Procedural sky with sun disc for reflections
- SSS approximation for light through thin wave crests
- Jacobian-driven foam at wave convergence zones
- Distance fog blending to hazy horizon

## References

- Rob Sherlock, "The Technical Art of Sea of Thieves" — GDC 2018
- Jerry Tessendorf, "Simulating Ocean Water" — SIGGRAPH 2001 course notes
- GPU Gems Chapter 1: "Effective Water Simulation from Physical Models"
- Mark Finch, projected grid technique for screen-space ocean mesh LOD
