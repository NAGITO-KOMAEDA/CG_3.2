#define CASCADES 4

cbuffer SceneConstants : register(b0)
{
    row_major float4x4 viewProjection;
    row_major float4x4 lightViewProjection[CASCADES];
    float4 cascadeEnds;
    float4 lightDirection;
    float4 eyePosition;
    float4 cameraForward;
    float4 options; // x: show cascade colors, y: enable PCF, z: map size
};

cbuffer ShadowConstants : register(b1)
{
    row_major float4x4 shadowViewProjection;
};

Texture2D diffuseMap : register(t0);
Texture2DArray<float> shadowMaps : register(t1);
SamplerState diffuseSampler : register(s0);
SamplerComparisonState shadowSampler : register(s1);

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct SceneVertexOutput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float depth : TEXCOORD3;
};

SceneVertexOutput SceneVS(VertexInput input)
{
    SceneVertexOutput result;
    float4 world = float4(input.position * 0.01f, 1.0f);
    float4 clip = mul(world, viewProjection);
    result.position = clip;
    result.worldPosition = world.xyz;
    result.normal = input.normal;
    result.uv = float2(input.uv.x, 1.0f - input.uv.y);
    result.depth = dot(world.xyz - eyePosition.xyz, cameraForward.xyz);
    return result;
}

struct ShadowVertexOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

ShadowVertexOutput ShadowVS(VertexInput input)
{
    ShadowVertexOutput result;
    result.position = mul(float4(input.position * 0.01f, 1.0f), shadowViewProjection);
    result.uv = float2(input.uv.x, 1.0f - input.uv.y);
    return result;
}

void ShadowPS(ShadowVertexOutput input)
{
    clip(diffuseMap.Sample(diffuseSampler, input.uv).a - 0.4f);
}

float ShadowVisibility(float3 worldPosition, float3 normal, int cascade)
{
    float4 projected = mul(float4(worldPosition, 1.0f), lightViewProjection[cascade]);
    float3 p = projected.xyz / projected.w;
    float2 uv = p.xy * float2(0.5f, -0.5f) + 0.5f;
    if (any(uv < 0.0f) || any(uv > 1.0f) || p.z < 0.0f || p.z > 1.0f)
        return 1.0f;

    float ndotl = saturate(dot(normal, -lightDirection.xyz));
    float compareDepth = p.z - (0.0004f + 0.0015f * (1.0f - ndotl));
    if (options.y < 0.5f)
    {
        // One unfiltered depth texel: a genuinely hard-edged reference mode.
        int2 pixel = clamp(int2(uv * options.z), 0, int(options.z) - 1);
        float storedDepth = shadowMaps.Load(int4(pixel, cascade, 0));
        return step(compareDepth, storedDepth);
    }

    float texel = 1.0f / options.z;
    float visibility = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            visibility += shadowMaps.SampleCmpLevelZero(
                shadowSampler, float3(uv + float2(x, y) * texel, cascade), compareDepth);
    return visibility / 9.0f;
}

float4 ScenePS(SceneVertexOutput input) : SV_TARGET
{
    float4 albedo = diffuseMap.Sample(diffuseSampler, input.uv);
    clip(albedo.a - 0.4f);

    int cascade = 0;
    if (input.depth > cascadeEnds.x) cascade = 1;
    if (input.depth > cascadeEnds.y) cascade = 2;
    if (input.depth > cascadeEnds.z) cascade = 3;

    float3 normal = normalize(input.normal);
    float lambert = saturate(dot(normal, -lightDirection.xyz));
    // Many Sponza cloth surfaces are two-sided.
    lambert = max(lambert, 0.30f * saturate(dot(-normal, -lightDirection.xyz)));
    float visibility = ShadowVisibility(input.worldPosition, normal, cascade);
    float3 color = albedo.rgb * (0.23f + 0.77f * lambert * visibility);

    if (options.x > 0.5f)
    {
        static const float3 colors[4] = {
            float3(1.0f, 0.45f, 0.45f), float3(0.45f, 1.0f, 0.45f),
            float3(0.45f, 0.65f, 1.0f), float3(1.0f, 0.9f, 0.4f)
        };
        color *= colors[cascade];
    }
    // Backbuffer is UNORM, so encode linear lighting for display.
    return float4(pow(saturate(color), 1.0f / 2.2f), 1.0f);
}
