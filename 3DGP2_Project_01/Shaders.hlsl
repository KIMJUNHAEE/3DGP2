cbuffer DrawConstants : register(b0) {
    row_major float4x4 worldViewProjection;
    row_major float4x4 world;
    float4 color;
    float animationProgress;
};

struct Vertex {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct Pixel {
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

Pixel VSMain(Vertex input) {
    Pixel output;
    output.position = mul(float4(input.position, 1), worldViewProjection);
    output.normal = mul(input.normal, (float3x3)world);
    output.uv = input.uv;
    return output;
}

float SurfaceLight(float3 normal) {
    const float3 light = normalize(float3(-0.42, 0.8, -0.35));
    const float diffuse = saturate(dot(normalize(normal), light));
    return 0.56 + 0.47 * diffuse;
}

float4 PSMain(Pixel input) : SV_TARGET {
    return float4(color.rgb * SurfaceLight(input.normal), color.a);
}

Texture2D surfaceTexture : register(t0);
SamplerState surfaceSampler : register(s0);

float4 PSFont(Pixel input) : SV_TARGET {
    return surfaceTexture.Sample(surfaceSampler, input.uv) * color;
}

float4 PSModel(Pixel input) : SV_TARGET {
    const float3 baseColor = surfaceTexture.Sample(surfaceSampler, input.uv).rgb;
    return float4(baseColor * color.rgb * SurfaceLight(input.normal), 1);
}

struct ImpactPoint {
    float3 position : POSITION;
};

struct ImpactPixel {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

ImpactPoint VSImpact(float3 position : POSITION) {
    ImpactPoint output;
    output.position = position;
    return output;
}

[maxvertexcount(4)]
void GSImpact(point ImpactPoint input[1], inout TriangleStream<ImpactPixel> stream) {
    const float2 corners[4] = {
        float2(0, 0), float2(0, 1),
        float2(1, 0), float2(1, 1)
    };
    for (uint i = 0; i < 4; ++i) {
        const float2 uv = corners[i];
        const float3 offset = float3(uv.x - 0.5f, 0.5f - uv.y, 0);
        ImpactPixel output;
        output.position = mul(float4(input[0].position + offset, 1), worldViewProjection);
        output.uv = uv;
        stream.Append(output);
    }
}

float4 PSImpact(ImpactPixel input) : SV_TARGET {
    // 4×4 텍스처에서 경과 시간에 해당하는 프레임을 고른다.
    const uint frame = min((uint)(saturate(animationProgress) * 16.0f), 15u);
    const float2 tile = float2(frame % 4u, frame / 4u);
    const float2 atlasUV = (tile + input.uv) * 0.25f;
    return surfaceTexture.Sample(surfaceSampler, atlasUV);
}
