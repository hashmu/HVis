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
