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
