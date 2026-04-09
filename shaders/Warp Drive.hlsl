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
