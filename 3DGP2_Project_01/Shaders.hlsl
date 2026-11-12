cbuffer DrawConstants : register(b0) {
    row_major float4x4 worldViewProjection;
    row_major float4x4 world;
    float4 color;
};

struct Vertex {
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct Pixel {
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
};

Pixel VSMain(Vertex input) {
    Pixel output;
    output.position = mul(float4(input.position, 1), worldViewProjection);
    output.normal = mul(input.normal, (float3x3)world);
    return output;
}

float4 PSMain(Pixel input) : SV_TARGET {
    const float3 light = normalize(float3(-0.42, 0.8, -0.35));
    const float diffuse = saturate(dot(normalize(input.normal), light));
    return float4(color.rgb * (0.56 + 0.47 * diffuse), color.a);
}
