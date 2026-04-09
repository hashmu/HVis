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
