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
