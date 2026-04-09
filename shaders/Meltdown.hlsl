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
