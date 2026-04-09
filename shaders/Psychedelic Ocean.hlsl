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
