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
// Shader: Brick Maze
// ============================================================
static const char* g_psMaze = R"(
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

// Hash functions
float hash1(float2 p) {
    p = frac(p * float2(443.897, 441.423));
    p += dot(p, p + 19.19);
    return frac(p.x * p.y);
}

float hash1f(float n) {
    return frac(sin(n) * 43758.5453);
}

float2 hash2(float2 p) {
    return float2(hash1(p), hash1(p + float2(127.1, 311.7)));
}

// SDF primitives
float sdBox(float3 p, float3 b) {
    float3 d = abs(p) - b;
    return min(max(d.x, max(d.y, d.z)), 0.0) + length(max(d, 0.0));
}

// Brick pattern: returns 0-1 mortar/brick and brick UV
float2 brickUV(float2 p) {
    float row = floor(p.y * 4.0);
    float offset = fmod(row, 2.0) * 0.5;
    float2 brick = float2(frac(p.x * 2.0 + offset), frac(p.y * 4.0));
    return brick;
}

float brickPattern(float2 p) {
    float row = floor(p.y * 4.0);
    float offset = fmod(row, 2.0) * 0.5;
    float2 brick = float2(frac(p.x * 2.0 + offset), frac(p.y * 4.0));
    float2 mortar = smoothstep(0.0, 0.06, brick) * smoothstep(0.0, 0.06, 1.0 - brick);
    return mortar.x * mortar.y;
}

// Maze layout: determines if a cell has a wall
float mazeWall(float2 cell) {
    // Keep center 3 columns clear for camera path (x cells -1, 0, 1)
    if (abs(cell.x) <= 1.0) return 0.0;
    float h = hash1(cell);
    float passage = hash1(float2(cell.y, 0.0));
    if (passage < 0.3) return 0.0;
    return h < 0.45 ? 1.0 : 0.0;
}

// Scene SDF
float map(float3 p) {
    // Floor and ceiling
    float d = min(p.y + 1.0, 2.5 - p.y);

    // Maze walls on a grid
    float cellSize = 3.0;
    float2 cell = floor(p.xz / cellSize);
    float2 local = fmod(p.xz, cellSize) - cellSize * 0.5;
    // Fix negative modulo
    if (p.x < 0.0) { cell.x -= 1.0; local.x = fmod(p.x, cellSize) + cellSize * 0.5; }

    // Check this cell and neighbors for walls
    for (int dx = -1; dx <= 1; dx++) {
        for (int dz = -1; dz <= 1; dz++) {
            float2 nc = cell + float2(dx, dz);
            if (mazeWall(nc) > 0.5) {
                float3 wallCenter = float3(
                    (nc.x + 0.5) * cellSize,
                    0.75,
                    (nc.y + 0.5) * cellSize
                );
                // Wall block with slight height variation
                float hVar = hash1(nc * 7.0) * 0.5;
                float wallDist = sdBox(p - wallCenter,
                    float3(cellSize * 0.45, 1.5 + hVar, cellSize * 0.45));
                d = min(d, wallDist);
            }
        }
    }

    // Audio-reactive wall undulation (visual only, fades near camera corridor)
    float corridorDist = abs(p.x);
    float undulationMask = smoothstep(3.0, 6.0, corridorDist);
    float wave = sin(p.z * 0.5 + time * 2.0) * bass * 0.15;
    d += wave * 0.3 * undulationMask;

    return d;
}

// Normal via gradient
float3 calcNormal(float3 p) {
    float2 e = float2(0.01, 0.0);
    return normalize(float3(
        map(p + e.xyy) - map(p - e.xyy),
        map(p + e.yxy) - map(p - e.yxy),
        map(p + e.yyx) - map(p - e.yyx)
    ));
}

float4 main(float4 screenPos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 p = (uv - 0.5) * float2(resolution.x / resolution.y, 1.0);

    // Camera: constant forward speed, gentle sway within safe corridor
    float camZ = time * 3.0;
    float camY = 0.5 + sin(time * 2.5) * 0.05;
    float camX = sin(time * 0.7) * 1.5;

    float3 ro = float3(camX, camY, camZ);

    // Look direction with treble-driven head tilt
    float tilt = sin(time * 1.3) * treble * 0.05;
    float3 forward = float3(sin(time * 0.3) * 0.15, tilt, 1.0);
    forward = normalize(forward);

    // Camera basis
    float3 right = normalize(cross(float3(0, 1, 0), forward));
    float3 up = cross(forward, right);

    float3 rd = normalize(p.x * right + p.y * up + 1.5 * forward);

    // Raymarch
    float t = 0.0;
    float dist = 0.0;
    int i;
    for (i = 0; i < 80; i++) {
        float3 pos = ro + rd * t;
        dist = map(pos);
        if (dist < 0.005 || t > 60.0) break;
        t += dist * 0.8; // slight understep for stability
    }

    float3 col = float3(0.0, 0.0, 0.0);

    if (t < 60.0) {
        float3 pos = ro + rd * t;
        float3 n = calcNormal(pos);

        // Brick texture
        float2 wallUV;
        if (abs(n.y) > 0.8) {
            wallUV = pos.xz * 0.3; // floor/ceiling
        } else if (abs(n.x) > abs(n.z)) {
            wallUV = pos.zy * 0.3; // X-facing wall
        } else {
            wallUV = pos.xy * 0.3; // Z-facing wall
        }

        float brick = brickPattern(wallUV);
        float2 bUV = brickUV(wallUV);

        // Brick color variation
        float2 brickCell = floor(wallUV * float2(2.0, 4.0));
        float brickVar = hash1(brickCell) * 0.3;

        // Base brick color — warm tones with frequency-driven tint
        float3 brickCol = float3(0.55 + brickVar, 0.25 + brickVar * 0.5, 0.15);
        // Mid frequencies shift wall hue
        brickCol.r += mid * 0.15;
        brickCol.b += treble * 0.2;

        float3 mortarCol = float3(0.35, 0.33, 0.30);
        float3 surfaceCol = lerp(mortarCol, brickCol, brick);

        // Lighting
        // Main light follows camera
        float3 lightPos = ro + float3(0.0, 1.5, 2.0);
        float3 toLight = lightPos - pos;
        float lightDist = length(toLight);
        float3 lightDir = toLight / lightDist;

        float diff = max(dot(n, lightDir), 0.0);
        float atten = 1.0 / (1.0 + lightDist * 0.05 + lightDist * lightDist * 0.01);

        // Specular
        float3 halfVec = normalize(lightDir - rd);
        float spec = pow(max(dot(n, halfVec), 0.0), 32.0) * 0.5;

        // Frequency-reactive colored point lights along the corridor
        float3 freqLight = float3(0.0, 0.0, 0.0);
        for (int li = 0; li < 4; li++) {
            float lz = camZ + (float)(li + 1) * 6.0;
            float lx = sin(lz * 0.3) * 1.5;
            float3 lp = float3(lx, 2.0, lz);
            float3 toLp = lp - pos;
            float ld = length(toLp);
            float la = 1.0 / (1.0 + ld * ld * 0.08);

            int bandIdx = (li * 4) % 16;
            float bandVal = bands[bandIdx];

            // Each light gets a different color from bands
            float3 lc;
            if (li == 0) lc = float3(1.0, 0.3, 0.1);
            else if (li == 1) lc = float3(0.1, 0.5, 1.0);
            else if (li == 2) lc = float3(0.1, 1.0, 0.3);
            else lc = float3(1.0, 0.1, 0.8);

            float lDiff = max(dot(n, normalize(toLp)), 0.0);
            freqLight += lc * lDiff * la * bandVal * 3.0;
        }

        // Ambient
        float3 ambient = float3(0.06, 0.05, 0.07) * (1.0 + energy * 0.5);

        col = surfaceCol * (ambient + diff * atten * float3(1.0, 0.9, 0.7))
            + spec * atten * float3(1.0, 0.95, 0.8)
            + surfaceCol * freqLight;

        // Bass pulse: bricks glow from within
        float glowPulse = brick * bass * 0.3;
        col += float3(0.4, 0.1, 0.05) * glowPulse;

        // Edge glow on mortar lines from energy
        float mortarGlow = (1.0 - brick) * energy * 0.2;
        col += float3(0.1, 0.2, 0.5) * mortarGlow;

    }

    // Fog — color shifts with audio
    float fogDist = 1.0 - exp(-t * 0.025);
    float3 fogCol = float3(0.02, 0.02, 0.04)
        + float3(0.05, 0.0, 0.1) * bass
        + float3(0.0, 0.05, 0.05) * mid;
    col = lerp(col, fogCol, fogDist);

    // Vignette
    float2 vc = uv - 0.5;
    float vignette = 1.0 - dot(vc, vc) * 1.5;
    col *= smoothstep(0.0, 1.0, vignette);

    return float4(col, 1.0);
}
)";

// ============================================================
// Shader: Psychedelic Ocean
// ============================================================
static const char* g_psOcean = R"(
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

// Multi-octave wave noise
float waveHash(float2 p) {
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float waveNoise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = waveHash(i);
    float b = waveHash(i + float2(1, 0));
    float c = waveHash(i + float2(0, 1));
    float d = waveHash(i + float2(1, 1));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float oceanFBM(float2 p, float t) {
    float val = 0.0;
    float amp = 0.5;
    float freq = 1.0;
    for (int i = 0; i < 5; i++) {
        val += amp * waveNoise(p * freq + float2(t * 0.3 * freq, t * 0.2));
        p = float2(p.x * 0.866 - p.y * 0.5, p.x * 0.5 + p.y * 0.866);
        amp *= 0.5;
        freq *= 2.1;
    }
    return val;
}

// Ocean height at world position
float oceanHeight(float2 xz) {
    float t = time;

    // Base waves
    float h = 0.0;

    // Large rolling swells
    h += sin(xz.x * 0.3 + t * 0.8) * 0.4;
    h += sin(xz.y * 0.2 + t * 0.5 + 1.5) * 0.3;
    h += sin((xz.x + xz.y) * 0.15 + t * 0.6) * 0.25;

    // Detailed wave noise
    h += (oceanFBM(xz * 0.08, t) - 0.5) * 1.5;

    // Bass makes big swells
    h *= 0.8 + bass * 0.6;

    // Mid adds choppiness
    h += sin(xz.x * 1.5 + t * 3.0) * mid * 0.15;
    h += sin(xz.y * 1.8 + t * 2.5) * mid * 0.12;

    // Frequency bands add detail ripples
    for (int i = 0; i < 4; i++) {
        float bandVal = bands[i * 4];
        float angle = (float)i * 1.57;
        float2 dir = float2(cos(angle), sin(angle));
        h += sin(dot(xz, dir) * (3.0 + i * 2.0) + t * (2.0 + i)) * bandVal * 0.08;
    }

    return h;
}

// Normal from height field
float3 oceanNormal(float2 xz) {
    float e = 0.1;
    float h = oceanHeight(xz);
    float hx = oceanHeight(xz + float2(e, 0));
    float hz = oceanHeight(xz + float2(0, e));
    return normalize(float3(h - hx, e, h - hz));
}

float4 main(float4 screenPos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 p = (uv - 0.5) * float2(resolution.x / resolution.y, 1.0);

    // Camera
    float camHeight = 3.0 + sin(time * 0.3) * 0.5;
    float3 ro = float3(0.0, camHeight, time * 2.0);
    float3 lookAt = ro + float3(sin(time * 0.1) * 2.0, -0.5, 10.0);

    // Camera basis
    float3 fwd = normalize(lookAt - ro);
    float3 right = normalize(cross(float3(0, 1, 0), fwd));
    float3 up = cross(fwd, right);
    float3 rd = normalize(p.x * right - p.y * up + 1.8 * fwd);

    // --- Sky / Horizon ---
    // Psychedelic sky dome
    float skyT = rd.y * 0.5 + 0.5;
    float3 skyCol = pal(skyT * 0.5 + time * 0.03,
        float3(0.5, 0.5, 0.5), float3(0.5, 0.5, 0.5),
        float3(1.0, 0.7, 0.4), float3(0.0 + treble * 0.2, 0.15, 0.2 + bass * 0.1));

    // Aurora / horizon bands
    float horizon = exp(-abs(rd.y) * 8.0);
    float3 horizonCol = pal(time * 0.05 + rd.x * 0.3,
        float3(0.5, 0.5, 0.5), float3(0.5, 0.5, 0.5),
        float3(1.0, 1.0, 1.0), float3(0.0, 0.33, 0.67));
    horizonCol *= (1.0 + energy * 2.0);
    skyCol += horizonCol * horizon * 0.8;

    // Psychedelic sun
    float3 sunDir = normalize(float3(
        sin(time * 0.05) * 0.3,
        0.25 + sin(time * 0.08) * 0.1,
        1.0
    ));
    float sunDot = max(dot(rd, sunDir), 0.0);
    float3 sunCol = pal(time * 0.02,
        float3(1.0, 0.8, 0.6), float3(0.3, 0.3, 0.3),
        float3(1.0, 1.0, 0.5), float3(0.0, 0.1, 0.2));
    skyCol += sunCol * pow(sunDot, 64.0) * 2.0;
    skyCol += sunCol * pow(sunDot, 8.0) * 0.3;

    float3 col = skyCol;

    // --- Ocean raymarching ---
    // Only march rays that actually point downward toward the water
    if (rd.y < -0.001) {
        // Flat plane intersection at y=0
        float tFlat = -ro.y / rd.y;
        tFlat = clamp(tFlat, 1.0, 800.0);

        // Step along ray to find ocean surface
        float t = min(tFlat * 0.2, 5.0);
        float tMax = min(tFlat * 2.0, 800.0);
        bool hit = false;
        float3 hitPos = float3(0, 0, 0);

        for (int i = 0; i < 48; i++) {
            if (t > tMax) break;
            float3 pos = ro + rd * t;
            float h = oceanHeight(pos.xz);
            if (pos.y < h) {
                // Binary refine
                float tA = t - (t * 0.03);
                float tB = t;
                for (int j = 0; j < 6; j++) {
                    float tM = (tA + tB) * 0.5;
                    float3 pm = ro + rd * tM;
                    if (pm.y < oceanHeight(pm.xz)) tB = tM;
                    else tA = tM;
                }
                t = (tA + tB) * 0.5;
                hitPos = ro + rd * t;
                hit = true;
                break;
            }
            // Adaptive step: small near camera, larger in the distance
            float stepSize = max(0.3, (pos.y - h) * 0.4 + t * 0.01);
            t += stepSize;
        }

        // For near-horizon rays that didn't hit geometry, fake a hit at the flat plane
        if (!hit) {
            hitPos = ro + rd * tFlat;
            hit = true;
            t = tFlat;
        }

        if (hit) {
            float3 n = oceanNormal(hitPos.xz);

            // Fresnel
            float cosTheta = max(dot(-rd, n), 0.0);
            float fresnel = pow(1.0 - cosTheta, 5.0);
            fresnel = lerp(0.04, 1.0, fresnel);

            // Reflection — sample sky
            float3 refl = reflect(rd, n);
            float reflSkyT = refl.y * 0.5 + 0.5;
            float3 reflCol = pal(reflSkyT * 0.5 + time * 0.03,
                float3(0.5, 0.5, 0.5), float3(0.5, 0.5, 0.5),
                float3(1.0, 0.7, 0.4), float3(0.0 + treble * 0.2, 0.15, 0.2 + bass * 0.1));
            // Sun reflection
            float reflSun = pow(max(dot(refl, sunDir), 0.0), 128.0);
            reflCol += sunCol * reflSun * 3.0;

            // Psychedelic water color — shifts with audio
            float3 deepCol = pal(hitPos.z * 0.01 + time * 0.05 + bass * 0.3,
                float3(0.0, 0.1, 0.2), float3(0.3, 0.3, 0.4),
                float3(1.0, 0.8, 0.6), float3(0.0 + mid * 0.2, 0.3, 0.6 + treble * 0.2));

            // Subsurface scattering — waves lit from behind
            float sss = pow(max(dot(rd, sunDir), 0.0), 4.0);
            sss *= max(0.0, 0.5 - n.y) * 2.0;
            float3 sssCol = float3(0.1, 0.5, 0.4) * sss * (1.0 + energy);

            // Combine
            col = lerp(deepCol + sssCol, reflCol, fresnel);

            // Foam on wave peaks
            float h = oceanHeight(hitPos.xz);
            float foam = smoothstep(0.5, 1.2, h) * (0.5 + treble * 0.5);
            col = lerp(col, float3(0.9, 0.95, 1.0), foam * 0.4);

            // Band-reactive caustic patterns on surface
            float caustic = 0.0;
            for (int ci = 0; ci < 3; ci++) {
                float bv = bands[ci * 3 + 8];
                float2 cuv = hitPos.xz * (2.0 + ci) + time * float2(0.5, 0.3);
                caustic += (sin(cuv.x * 3.0) * sin(cuv.y * 3.0) * 0.5 + 0.5) * bv;
            }
            col += caustic * float3(0.1, 0.15, 0.2) * 0.4;

            // Distance fog to horizon
            float dist = length(hitPos - ro);
            float fog = 1.0 - exp(-dist * 0.003);
            float3 fogCol = lerp(
                float3(0.05, 0.08, 0.15),
                horizonCol * 0.3,
                fog
            );
            col = lerp(col, fogCol, fog);
        }
    }

    // Vignette
    float2 vc = uv - 0.5;
    float vignette = 1.0 - dot(vc, vc) * 1.2;
    col *= smoothstep(0.0, 1.0, vignette);

    // Overall energy brightness
    col *= 0.8 + energy * 0.4;

    return float4(saturate(col), 1.0);
}
)";

// ============================================================
// Shader: Warp Drive
// ============================================================
static const char* g_psWarp = R"(
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

float hash(float n) { return frac(sin(n) * 43758.5453); }
float hash2to1(float2 p) {
    p = frac(p * float2(443.897, 441.423));
    p += dot(p, p + 19.19);
    return frac(p.x * p.y);
}

// Star field layer — returns brightness and depth
float starField(float2 uv, float layer, float speed) {
    float2 cell = floor(uv);
    float2 local = frac(uv) - 0.5;

    float bright = 0.0;
    // Check neighbors for closest star
    for (int x = -1; x <= 1; x++) {
        for (int y = -1; y <= 1; y++) {
            float2 nc = cell + float2(x, y);
            float h = hash2to1(nc + layer * 100.0);
            if (h > 0.7) { // ~30% of cells have stars
                float2 starPos = float2(hash2to1(nc * 1.3 + layer), hash2to1(nc * 2.7 + layer)) - 0.5;
                float dist = length(local - float2(x, y) - starPos);
                float size = (0.01 + hash2to1(nc * 3.1 + layer) * 0.03) / (0.5 + layer * 0.3);
                float twinkle = 0.7 + 0.3 * sin(time * (3.0 + hash(nc.x + nc.y * 37.0 + layer) * 5.0) + h * 20.0);
                bright += smoothstep(size, 0.0, dist) * twinkle;
            }
        }
    }
    return bright;
}

// Sphere SDF for planets
float sdSphere(float3 p, float r) { return length(p) - r; }

float4 main(float4 screenPos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 p = (uv - 0.5) * float2(resolution.x / resolution.y, 1.0);

    // Camera flying forward
    float camZ = time * 20.0;
    float3 ro = float3(0.0, 0.0, camZ);

    // Slight drift
    float3 rd = normalize(float3(p, -1.5));

    // Roll with mid
    float roll = sin(time * 0.2) * 0.1 + mid * 0.15;
    float cr = cos(roll), sr = sin(roll);
    rd.xy = float2(rd.x * cr - rd.y * sr, rd.x * sr + rd.y * cr);

    float3 col = float3(0.0, 0.0, 0.0);

    // === Deep space background ===
    // Nebula fog
    float2 nebulaUV = p * 0.5 + float2(time * 0.01, time * 0.008);
    float neb = 0.0;
    float nebAmp = 0.5;
    for (int ni = 0; ni < 4; ni++) {
        neb += nebAmp * (sin(nebulaUV.x * (3.0 + ni) + nebulaUV.y * 2.0 + time * 0.1) * 0.5 + 0.5);
        nebulaUV *= 2.1;
        nebAmp *= 0.5;
    }
    neb = pow(neb, 2.0) * 0.3;
    float3 nebCol = pal(neb + time * 0.02,
        float3(0.1, 0.0, 0.1), float3(0.3, 0.2, 0.4),
        float3(1.0, 0.7, 0.4), float3(0.0, 0.15 + bass * 0.1, 0.3));
    col += nebCol * neb * (0.5 + energy * 0.5);

    // === Star layers (parallax depth) ===
    for (int sl = 0; sl < 4; sl++) {
        float depth = 1.0 + (float)sl * 1.5;
        float speed = 0.2 / depth;
        float2 starUV = p * (8.0 + sl * 6.0) + float2(0.0, camZ * speed);
        float stars = starField(starUV, (float)sl, speed);

        float bandVal = bands[sl * 3];
        float3 starCol = pal((float)sl * 0.25 + time * 0.05 + bandVal,
            float3(0.8, 0.8, 0.9), float3(0.2, 0.2, 0.2),
            float3(1.0, 0.7, 0.4), float3(0.0 + treble * 0.1, 0.1, 0.3));
        col += starCol * stars * (0.5 + bandVal * 1.5);
    }

    // === Warp streaks ===
    float warpSpeed = 3.0 + bass * 8.0;
    float2 warpCenter = p;
    float warpAngle = atan2(warpCenter.y, warpCenter.x);
    float warpDist = length(warpCenter);

    // Multiple streak layers
    for (int wi = 0; wi < 3; wi++) {
        float layer = (float)wi;
        float angleSlots = 60.0 + layer * 30.0;
        float slot = floor(warpAngle * angleSlots / 6.28318);
        float slotHash = hash(slot + layer * 100.0);

        // Streak visibility based on hash and radius
        float streakR = 0.05 + slotHash * 0.4 + layer * 0.15;
        float streak = smoothstep(streakR + 0.15, streakR, warpDist)
                     * smoothstep(streakR - 0.02, streakR, warpDist);

        // Animate streak length with speed
        float streakLen = 0.02 + warpSpeed * 0.01;
        float radialPos = frac(warpDist * 3.0 - time * (2.0 + layer) + slotHash * 5.0);
        streak *= smoothstep(0.0, streakLen, radialPos) * smoothstep(streakLen * 2.0, streakLen, radialPos);

        float bandIdx = bands[(int)(slotHash * 15.0) % 16];
        float3 streakCol = pal(slotHash + time * 0.1 + layer * 0.3,
            float3(0.5, 0.5, 0.7), float3(0.5, 0.4, 0.5),
            float3(1.0, 0.8, 0.6), float3(0.0, 0.2, 0.5));
        col += streakCol * streak * (0.3 + bandIdx * 2.0) * warpSpeed * 0.1;
    }

    // === Planets whizzing by ===
    for (int pi = 0; pi < 3; pi++) {
        // Each planet has a fixed position in world space, repeats
        float spacing = 80.0;
        float offset = (float)pi * spacing / 3.0;
        float planetZ = fmod(camZ + offset, spacing) - spacing * 0.5;

        float planetSeed = floor((camZ + offset) / spacing) + (float)pi * 17.0;
        float px = (hash(planetSeed * 1.3) - 0.5) * 4.0;
        float py = (hash(planetSeed * 2.7) - 0.5) * 2.0;
        float planetR = 0.3 + hash(planetSeed * 3.1) * 1.2;

        float3 planetPos = float3(px, py, camZ - planetZ) - ro;
        // Project to screen
        if (planetPos.z < 0.0) { // behind camera
            float projScale = -1.5 / planetPos.z;
            float2 projPos = planetPos.xy * projScale;
            float projR = planetR * projScale;

            float dist = length(p - projPos);
            if (dist < projR * 1.5) {
                // Sphere shading
                float sphereDist = dist / projR;
                if (sphereDist < 1.0) {
                    // Normal on sphere
                    float2 sphereUV = (p - projPos) / projR;
                    float sz = sqrt(1.0 - dot(sphereUV, sphereUV));
                    float3 sn = normalize(float3(sphereUV, sz));

                    // Planet color — psychedelic, based on seed
                    float3 planetCol = pal(hash(planetSeed) + sn.x * 0.3 + time * 0.03,
                        float3(0.5, 0.5, 0.5), float3(0.5, 0.5, 0.5),
                        float3(1.0, 0.8, 0.6), float3(hash(planetSeed * 5.0), 0.2, 0.5));

                    // Surface bands / storms
                    float bands_pattern = sin(sn.y * 8.0 + hash(planetSeed * 7.0) * 10.0 + time * 0.5) * 0.5 + 0.5;
                    planetCol = lerp(planetCol, planetCol * 1.5, bands_pattern * 0.3);

                    // Lighting
                    float3 lightDir = normalize(float3(0.5, 0.8, -0.3));
                    float diff = max(dot(sn, lightDir), 0.0) * 0.8 + 0.2;
                    float spec = pow(max(dot(reflect(-lightDir, sn), float3(0, 0, 1)), 0.0), 16.0);

                    // Atmosphere glow
                    float atmo = pow(1.0 - sz, 3.0);
                    float3 atmoCol = pal(hash(planetSeed * 11.0) + time * 0.05,
                        float3(0.3, 0.4, 0.6), float3(0.3, 0.3, 0.3),
                        float3(1.0, 1.0, 0.5), float3(0.0, 0.3, 0.6));

                    float3 pCol = planetCol * diff + spec * 0.4 + atmoCol * atmo * 0.6;
                    pCol *= 1.0 + bands[pi * 4] * 0.5;

                    col = lerp(col, pCol, smoothstep(1.0, 0.95, sphereDist));
                }

                // Outer atmosphere glow
                float glowDist = max(0.0, sphereDist - 0.8) / 0.7;
                if (sphereDist > 0.8 && sphereDist < 1.5) {
                    float glow = exp(-glowDist * 3.0) * 0.5;
                    float3 glowCol = pal(hash(planetSeed * 11.0) + time * 0.05,
                        float3(0.3, 0.4, 0.6), float3(0.3, 0.3, 0.3),
                        float3(1.0, 1.0, 0.5), float3(0.0, 0.3, 0.6));
                    col += glowCol * glow * (1.0 + energy);
                }
            }
        }
    }

    // === Central engine glow ===
    float engineDist = length(p);
    float engineGlow = exp(-engineDist * 4.0) * (0.3 + bass * 0.7);
    float3 engineCol = pal(time * 0.1 + bass,
        float3(0.4, 0.5, 0.8), float3(0.4, 0.3, 0.3),
        float3(1.0, 0.8, 0.4), float3(0.0, 0.15, 0.4));
    col += engineCol * engineGlow;

    // === Speed lines at edges (stronger with bass) ===
    float edgeDist = max(abs(p.x) / (resolution.x / resolution.y), abs(p.y));
    float speedLine = smoothstep(0.3, 0.5, edgeDist);
    float lineAngle = atan2(p.y, p.x);
    float linePattern = pow(abs(sin(lineAngle * 40.0 + time * 10.0)), 8.0);
    col += float3(0.3, 0.4, 0.8) * speedLine * linePattern * bass * 0.5;

    // Vignette
    float2 vc = uv - 0.5;
    float vignette = 1.0 - dot(vc, vc) * 1.5;
    col *= smoothstep(0.0, 1.0, vignette);

    col *= 0.8 + energy * 0.4;

    return float4(saturate(col), 1.0);
}
)";

// ============================================================
// Shader: Meltdown (metal / industrial)
// ============================================================
static const char* g_psMetal = R"(
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

float hash(float2 p) {
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

// Hard gate: snaps to 0 or 1 with adjustable threshold
float gate(float v, float thresh) {
    return step(thresh, v);
}

// Hexagonal grid
float2 hexGrid(float2 p, out float2 cell) {
    float2 q = float2(p.x * 1.1547, p.y + p.x * 0.5774);
    float2 pi = floor(q);
    float2 pf = frac(q);
    float v = fmod(pi.x + pi.y, 3.0);
    float ca = step(1.0, v);
    float cb = step(2.0, v);
    float2 ma = step(pf.xy, pf.yx);
    cell = pi + ca - cb * ma;
    return pf;
}

float4 main(float4 screenPos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 p = (uv - 0.5) * float2(resolution.x / resolution.y, 1.0);
    float r = length(p);
    float a = atan2(p.y, p.x);

    // === HARD-GATED SIGNALS ===
    // These snap on/off like a noise gate — no smooth ramps
    float bassGate = gate(bass, 0.25);      // kicks, bass drops
    float midGate = gate(mid, 0.2);         // guitar chugs, snare
    float trebGate = gate(treble, 0.15);    // cymbals, hi-hat
    float slamGate = gate(bass, 0.5);       // only the hardest hits

    // Sub-bass: bands 0-3, low-mid: 4-7, mid: 8-15, presence: 16-23, air: 24-31
    float kick = gate(bands[0] + bands[1], 0.4);
    float snare = gate(bands[8] + bands[10], 0.35);
    float guitar = (bands[4] + bands[5] + bands[6] + bands[7]) * 0.25;
    float guitarGate = gate(guitar, 0.15);

    // === COLOR THEME — shifts over time and with dominant frequency ===
    // Slowly cycles hue, guitar/bass/treble bias the palette
    float hueShift = time * 0.04 + bass * 0.2 - treble * 0.15;
    float3 palA = 0.5 + 0.5 * cos(6.28318 * (float3(0.0, 0.33, 0.67) + hueShift));
    float3 palB = 0.5 + 0.5 * cos(6.28318 * (float3(0.1, 0.4, 0.75) + hueShift + 0.3));
    float3 palC = 0.5 + 0.5 * cos(6.28318 * (float3(0.2, 0.5, 0.8) + hueShift + 0.6));

    float3 col = float3(0.0, 0.0, 0.0);

    // === SHOCKWAVE RINGS (bass/kick hits) ===
    for (int ri = 0; ri < 4; ri++) {
        float ringSpeed = 2.0 + bass * 3.0 + (float)ri * 0.5;
        float ringPhase = frac(time * ringSpeed * 0.3 + (float)ri * 0.25);
        float ringR = ringPhase * (0.8 + bass * 0.6);
        float ringWidth = 0.01 + bass * 0.025;
        float ring = smoothstep(ringWidth, 0.0, abs(r - ringR));
        ring *= (1.0 - ringPhase);
        ring *= bass * 1.5;

        float3 ringCol = lerp(palA, palB, ringPhase + (float)ri * 0.2);
        col += ringCol * ring * 2.5;
    }

    // === HEXAGONAL GRID (rotates and breathes) ===
    float gridRot = time * 0.15 + mid * 0.3;
    float gc = cos(gridRot), gs = sin(gridRot);
    float2 gridP = float2(p.x * gc - p.y * gs, p.x * gs + p.y * gc);
    float gridScale = 5.0 + sin(time * 0.5) * 1.0 + bass * 1.0;
    float2 hexCell;
    float2 hexUV = hexGrid(gridP * gridScale, hexCell);
    float hexHash = hash(hexCell);
    float hexDist = length(hexUV - 0.5);

    // Grid lines
    float gridLine = smoothstep(0.02, 0.0, abs(hexDist - 0.45));
    float3 gridCol = palA * 0.15 + palB * 0.4 * guitarGate;
    col += gridCol * gridLine;

    // Hex cells light up from frequency bands
    int bandIdx = (int)(hexHash * 16.0) % 16;
    float bandVal = bands[bandIdx];
    float cellGate = gate(bandVal, 0.12);
    float cellBright = cellGate * bandVal * 3.0;

    // Color mapped to band position across the palette
    float bandT = (float)bandIdx / 16.0;
    float3 cellCol = 0.5 + 0.5 * cos(6.28318 * (float3(0.0, 0.33, 0.67) + hueShift + bandT));
    // Intensify toward white at high energy
    cellCol = lerp(cellCol, float3(1.0, 1.0, 1.0), saturate(bandVal - 0.5));

    float cellFill = smoothstep(0.45, 0.2, hexDist);
    col += cellCol * cellFill * cellBright;

    // === SLAM FLASH ===
    float slam = slamGate * (1.0 - r * 1.5);
    float3 slamCol = lerp(palC, float3(1.0, 1.0, 1.0), 0.5);
    col += slamCol * max(0.0, slam) * 0.6;

    // === ANGULAR SHARDS (snare) ===
    float shardCount = 12.0;
    float shardAngle = frac(a / 6.28318 * shardCount);
    float shard = step(0.4, shardAngle) * step(shardAngle, 0.6);
    float shardPulse = frac(r * 3.0 - time * 4.0);
    shardPulse = step(0.7, shardPulse);
    col += palB * shard * shardPulse * snare * 1.5;

    // === SPECTRUM BARS (radial) ===
    float barCount = 32.0;
    float barAngle = frac(a / 6.28318 * barCount + 0.5);
    int barBand = (int)(frac(a / 6.28318 + 0.5 / barCount) * 32.0) % 32;
    float barVal = bands[barBand];
    float barGated = gate(barVal, 0.08);
    float barHeight = barVal * 0.6;
    float bar = step(0.15, barAngle) * step(barAngle, 0.85);
    bar *= step(r, barHeight) * step(0.02, r);
    // Color each bar by its position in the spectrum
    float barT = (float)barBand / 32.0;
    float3 barCol = 0.5 + 0.5 * cos(6.28318 * (float3(0.0, 0.33, 0.67) + hueShift + barT * 0.8));
    barCol = lerp(barCol, float3(1.0, 1.0, 1.0), saturate(barVal * 1.5 - 0.3));
    col += barCol * bar * barGated * 1.2;

    // === CENTER CORE ===
    float coreR = 0.03 + energy * 0.04 + bassGate * 0.02;
    float core = smoothstep(coreR, 0.0, r);
    float3 coreCol = lerp(palA, float3(1.0, 1.0, 1.0), energy);
    col += coreCol * core * 2.0;

    // Core halo
    float halo = exp(-r * 6.0) * (0.2 + energy * 0.6);
    col += palA * 0.6 * halo;

    // === STROBE on double-kick patterns ===
    float strobe = step(0.5, frac(time * 16.0)) * kick * 0.15;
    col += lerp(palC, float3(1.0, 1.0, 1.0), 0.6) * strobe;

    // === GUITAR CHUG edge distortion ===
    float edge = smoothstep(0.35, 0.5, r) * guitarGate;
    float edgeWave = sin(a * 8.0 + time * 6.0) * 0.5 + 0.5;
    col += palB * 0.6 * edge * edgeWave * guitar * 2.0;

    // === Treble sparkles (single-pixel, rare) ===
    float sparkle = hash(floor(uv * resolution) + floor(time * 30.0));
    sparkle = step(0.995, sparkle) * trebGate * treble;
    col += palC * sparkle * 1.5;

    // === Overall ===
    // Desaturate slightly when quiet, full color when loud
    float grey = dot(col, float3(0.299, 0.587, 0.114));
    col = lerp(float3(grey, grey, grey) * 0.5, col, 0.5 + energy * 0.5);

    // Vignette — tighter when loud
    float2 vc = uv - 0.5;
    float vigStr = 1.5 + bassGate * 0.5;
    float vignette = 1.0 - dot(vc, vc) * vigStr;
    col *= smoothstep(0.0, 1.0, vignette);

    return float4(saturate(col), 1.0);
}
)";

// ============================================================
// Shader: Ocean (Gerstner)
// ============================================================
static const char* g_psGerstner = R"(
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

static const int NUM_WAVES = 6;

// Pre-computed direction vectors (degrees -> radians -> cos/sin)
static const float2 waveDir[6] = {
    float2(1.0, 0.0),           // 0 deg
    float2(0.866, 0.5),         // 30 deg
    float2(0.9397, -0.342),     // -20 deg
    float2(0.6428, 0.766),      // 50 deg
    float2(0.7071, -0.7071),    // -45 deg
    float2(0.9659, 0.2588)      // 15 deg
};
static const float waveFreq[6]  = { 0.18, 0.30, 0.50, 0.80, 1.30, 2.00 };
static const float waveAmp[6]   = { 1.2, 0.7, 0.35, 0.18, 0.08, 0.04 };
static const float waveQ[6]     = { 0.6, 0.5, 0.7, 0.6, 0.5, 0.4 };
static const float waveSpeed[6] = { 0.8, 1.0, 1.3, 1.6, 2.0, 2.5 };

float gerstnerHeight(float2 xz, float a[6], float q[6]) {
    float h = 0.0;
    for (int i = 0; i < NUM_WAVES; i++) {
        float phase = dot(waveDir[i], xz) * waveFreq[i] + waveSpeed[i] * time;
        h += a[i] * cos(phase);
    }
    return h;
}

struct WaveResult {
    float height;
    float3 normal;
    float foam;
};

WaveResult evaluateWaves(float2 xz, float a[6], float q[6]) {
    WaveResult r;
    r.height = 0.0;
    float2 dh = float2(0.0, 0.0);
    float Jxx = 1.0, Jzz = 1.0, Jxz = 0.0;

    for (int i = 0; i < NUM_WAVES; i++) {
        float phase = dot(waveDir[i], xz) * waveFreq[i] + waveSpeed[i] * time;
        float c = cos(phase);
        float s = sin(phase);

        r.height += a[i] * c;

        float dPhase = -a[i] * waveFreq[i] * s;
        dh.x += waveDir[i].x * dPhase;
        dh.y += waveDir[i].y * dPhase;

        float qfa = q[i] * waveFreq[i] * a[i] * s;
        Jxx -= waveDir[i].x * waveDir[i].x * qfa;
        Jzz -= waveDir[i].y * waveDir[i].y * qfa;
        Jxz -= waveDir[i].x * waveDir[i].y * qfa;
    }

    r.normal = normalize(float3(-dh.x, 1.0, -dh.y));
    float J = Jxx * Jzz - Jxz * Jxz;
    // Only foam where waves strongly converge, with noise breakup
    float rawFoam = saturate((0.7 - J) * 2.5);
    float noiseBreak = frac(sin(dot(xz * 1.7, float2(127.1, 311.7))) * 43758.5453);
    r.foam = rawFoam * smoothstep(0.1, 0.5, noiseBreak + rawFoam * 0.5);

    return r;
}

float3 sky(float3 rd, float3 sunDir, float3 sunColor) {
    float3 zenith  = float3(0.08, 0.18, 0.4);
    float3 horizon = float3(0.3, 0.38, 0.5);
    float t = saturate(rd.y);
    float3 col = lerp(horizon, zenith, sqrt(t));

    float sunDot = max(dot(rd, sunDir), 0.0);
    col += sunColor * pow(sunDot, 512.0) * 1.5;
    col += sunColor * pow(sunDot, 64.0) * 0.05;
    return col;
}

float4 main(float4 screenPos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 p = (uv - 0.5) * float2(resolution.x / resolution.y, 1.0);

    // Audio-modulated wave parameters
    float a[6], q[6];
    for (int i = 0; i < NUM_WAVES; i++) {
        a[i] = waveAmp[i] * (0.6 + bands[i] * 1.0);
        q[i] = waveQ[i] * (0.8 + mid * 0.6);
        // Safety clamp: prevent Gerstner self-intersection
        float maxQ = 0.9 / max(waveFreq[i] * a[i], 0.001);
        q[i] = min(q[i], maxQ);
    }
    // Bass drives primary swells
    a[0] *= 1.0 + bass * 0.8;
    a[1] *= 1.0 + bass * 0.5;

    // Max possible wave height for raymarch bounds
    float maxH = 0.0;
    for (int m = 0; m < NUM_WAVES; m++) maxH += a[m];

    // Camera
    float camHeight = 4.0 + bass * 1.5 + sin(time * 0.2) * 0.5;
    float3 ro = float3(0.0, camHeight, time * 1.5);
    float3 lookAt = ro + float3(sin(time * 0.08) * 3.0, -1.5, 15.0);

    float3 fwd = normalize(lookAt - ro);
    float3 right = normalize(cross(float3(0, 1, 0), fwd));
    float3 up = cross(fwd, right);
    float3 rd = normalize(p.x * right - p.y * up + 1.8 * fwd);

    // Sun
    float3 sunDir = normalize(float3(1.5, 0.4, 0.5));
    float3 sunColor = float3(1.4, 1.2, 0.9);

    // Sky for upward rays
    float3 col = sky(rd, sunDir, sunColor);

    if (rd.y < -0.001) {
        // Flat-plane intersection at y=0
        float tFlat = -ro.y / rd.y;
        tFlat = clamp(tFlat, 1.0, 800.0);

        // Backtrack to account for wave height
        float tStart = max(1.0, tFlat - maxH / max(-rd.y, 0.01));
        float tMax = min(tFlat + maxH / max(-rd.y, 0.01), 800.0);

        bool hit = false;
        float t = tStart;
        float3 hitPos = float3(0, 0, 0);

        // Far LOD: skip march for distant water
        if (tFlat > 300.0) {
            hitPos = ro + rd * tFlat;
            hit = true;
            t = tFlat;
        } else {
            float step = (tMax - tStart) / 24.0;
            for (int i = 0; i < 24; i++) {
                float3 pos = ro + rd * t;
                float h = gerstnerHeight(pos.xz, a, q);
                if (pos.y < h) {
                    // Binary refinement
                    float tA = t - step;
                    float tB = t;
                    for (int j = 0; j < 4; j++) {
                        float tM = (tA + tB) * 0.5;
                        float3 pm = ro + rd * tM;
                        if (pm.y < gerstnerHeight(pm.xz, a, q)) tB = tM;
                        else tA = tM;
                    }
                    t = (tA + tB) * 0.5;
                    hitPos = ro + rd * t;
                    hit = true;
                    break;
                }
                t += step;
            }
            // Fallback to flat plane for near-horizon misses
            if (!hit) {
                hitPos = ro + rd * tFlat;
                hit = true;
                t = tFlat;
            }
        }

        if (hit) {
            WaveResult w = evaluateWaves(hitPos.xz, a, q);
            float3 n = w.normal;

            // Fresnel (Schlick, water IOR ~1.33 -> F0 ~0.02)
            float cosTheta = max(dot(-rd, n), 0.0);
            float fresnel = 0.02 + 0.98 * pow(1.0 - cosTheta, 5.0);

            // Reflection
            float3 refl = reflect(rd, n);
            float3 reflCol = sky(refl, sunDir, sunColor);

            // Sun specular on water
            float spec = pow(max(dot(refl, sunDir), 0.0), 256.0);
            reflCol += sunColor * spec * 3.0;

            // Deep water color
            float3 deepCol = float3(0.01, 0.05, 0.12);

            // SSS — light through thin wave crests
            float sss = pow(saturate(dot(rd, sunDir)), 4.0);
            sss *= pow(saturate(0.5 - n.y), 2.0) * 2.0;
            float3 sssCol = float3(0.0, 0.4, 0.3) * sss * (1.0 + energy);

            // Composite water
            col = lerp(deepCol + sssCol, reflCol, fresnel);

            // Foam from Jacobian
            float foam = w.foam * (0.5 + treble * 0.8);
            col = lerp(col, float3(0.85, 0.9, 0.95), saturate(foam) * 0.7);

            // Distance fog
            float dist = length(hitPos - ro);
            float fog = 1.0 - exp(-dist * 0.004);
            float3 fogCol = float3(0.45, 0.55, 0.65);
            col = lerp(col, fogCol, fog);
        }
    }

    // Vignette
    float2 vc = uv - 0.5;
    float vignette = 1.0 - dot(vc, vc) * 1.2;
    col *= smoothstep(0.0, 1.0, vignette);

    // Energy brightness
    col *= 0.7 + energy * 0.5;

    return float4(saturate(col), 1.0);
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
    if (CompileShader(g_psMaze, "main", &m_pixelShaders[m_shaderCount])) {
        m_shaderNames[m_shaderCount] = "Brick Maze";
        m_shaderCount++;
    }
    if (CompileShader(g_psOcean, "main", &m_pixelShaders[m_shaderCount])) {
        m_shaderNames[m_shaderCount] = "Ocean";
        m_shaderCount++;
    }
    if (CompileShader(g_psWarp, "main", &m_pixelShaders[m_shaderCount])) {
        m_shaderNames[m_shaderCount] = "Warp Drive";
        m_shaderCount++;
    }
    if (CompileShader(g_psMetal, "main", &m_pixelShaders[m_shaderCount])) {
        m_shaderNames[m_shaderCount] = "Meltdown";
        m_shaderCount++;
    }
    if (CompileShader(g_psGerstner, "main", &m_pixelShaders[m_shaderCount])) {
        m_shaderNames[m_shaderCount] = "Ocean (Gerstner)";
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
