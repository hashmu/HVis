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
