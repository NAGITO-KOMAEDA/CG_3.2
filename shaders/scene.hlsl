cbuffer SceneConstants : register(b0)
{
    row_major float4x4 gWorld;
    row_major float4x4 gView;
    row_major float4x4 gProjection;
    float4 gCameraPosition;
    float4 gLightPosition[4];
    float4 gLightColor[4];
    float4 gRenderParams; // normal Y sign, emissive intensity, unused, unused
};

Texture2D<float4> gAlbedoMap : register(t0);
Texture2D<float4> gNormalMap : register(t1);
SamplerState gMaterialSampler : register(s0);

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 texCoord : TEXCOORD0;
    float4 tangent : TANGENT;
};

struct PSInput
{
    float4 position : SV_Position;
    float3 worldPosition : POSITION0;
    float3 normal : NORMAL;
    float2 texCoord : TEXCOORD0;
    float4 tangent : TANGENT;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    const float4 worldPosition = mul(float4(input.position, 1.0), gWorld);
    output.worldPosition = worldPosition.xyz;
    output.position = mul(mul(worldPosition, gView), gProjection);
    output.normal = normalize(mul(float4(input.normal, 0.0), gWorld).xyz);
    output.tangent.xyz = normalize(mul(float4(input.tangent.xyz, 0.0), gWorld).xyz);
    output.tangent.w = input.tangent.w;
    output.texCoord = input.texCoord;
    return output;
}

float3 WorldNormal(PSInput input)
{
    const float3 n = normalize(input.normal);
    const float3 t = normalize(input.tangent.xyz - n * dot(n, input.tangent.xyz));
    const float3 b = normalize(cross(n, t) * input.tangent.w);
    float3 mapped = gNormalMap.Sample(gMaterialSampler, input.texCoord).xyz * 2.0 - 1.0;
    mapped.y *= gRenderParams.x;
    return normalize(mul(mapped, float3x3(t, b, n)));
}

float4 PSMain(PSInput input) : SV_Target
{
    const float4 albedoSample = gAlbedoMap.Sample(gMaterialSampler, input.texCoord);
    clip(albedoSample.a - 0.15);
    const float3 albedo = albedoSample.rgb;

    if (gRenderParams.y > 0.0)
        return float4(albedo * gRenderParams.y, 1.0);

    const float3 normal = WorldNormal(input);
    const float3 viewDirection = normalize(gCameraPosition.xyz - input.worldPosition);
    float3 color = albedo * 0.035;

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        const float3 toLight = gLightPosition[i].xyz - input.worldPosition;
        const float distanceSquared = max(dot(toLight, toLight), 0.04);
        const float3 lightDirection = toLight * rsqrt(distanceSquared);
        const float3 halfwayVector = normalize(lightDirection + viewDirection);
        const float nDotL = max(dot(normal, lightDirection), 0.0);
        const float specular = pow(max(dot(normal, halfwayVector), 0.0), 48.0);
        const float3 radiance = gLightColor[i].rgb / distanceSquared;
        color += (albedo * nDotL + 0.16 * specular) * radiance;
    }

    const float3 sunDirection = normalize(float3(-0.35, 0.80, -0.42));
    color += albedo * max(dot(normal, sunDirection), 0.0) * float3(1.15, 1.05, 0.92);
    return float4(color, 1.0);
}

