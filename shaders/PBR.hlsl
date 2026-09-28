static const float PI = 3.14159265359;

cbuffer SceneConstants : register(b0)
{
    row_major float4x4 gWorld;
    row_major float4x4 gView;
    row_major float4x4 gProjection;
    float4 gCameraPosition;
    float4 gLightPosition[4];
    float4 gLightColor[4];
    float4 gRenderParams; // exposure, IBL enabled, direct light enabled, normal Y sign
    float4 gDebugParams;  // debug mode, unused...
};

Texture2D<float4> gAlbedoMap : register(t0);
Texture2D<float4> gNormalMap : register(t1);
Texture2D<float4> gMetallicMap : register(t2);
Texture2D<float4> gRoughnessMap : register(t3);
TextureCube<float3> gIrradianceMap : register(t4);
TextureCube<float3> gPrefilteredMap : register(t5);
Texture2D<float2> gBrdfLut : register(t6);

SamplerState gMaterialSampler : register(s0);
SamplerState gEnvironmentSampler : register(s1);

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

float DistributionGGX(float3 normal, float3 halfwayVector, float roughness)
{
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float nDotH = max(dot(normal, halfwayVector), 0.0);
    const float nDotH2 = nDotH * nDotH;

    float denominator = nDotH2 * (a2 - 1.0) + 1.0;
    denominator = PI * denominator * denominator;
    return a2 / max(denominator, 0.000001);
}

float GeometrySchlickGGX(float nDotDirection, float roughness)
{
    const float r = roughness + 1.0;
    const float k = (r * r) / 8.0;
    return nDotDirection / max(nDotDirection * (1.0 - k) + k, 0.000001);
}

float GeometrySmith(float3 normal, float3 viewDirection, float3 lightDirection, float roughness)
{
    const float nDotV = max(dot(normal, viewDirection), 0.0);
    const float nDotL = max(dot(normal, lightDirection), 0.0);
    return GeometrySchlickGGX(nDotV, roughness) *
           GeometrySchlickGGX(nDotL, roughness);
}

float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(saturate(1.0 - cosTheta), 5.0);
}

float3 FresnelSchlickRoughness(float cosTheta, float3 f0, float roughness)
{
    return f0 + (max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), f0) - f0) *
                pow(saturate(1.0 - cosTheta), 5.0);
}

float3 GetWorldNormal(PSInput input)
{
    const float3 geometricNormal = normalize(input.normal);
    const float3 tangent = normalize(input.tangent.xyz -
                                     geometricNormal * dot(geometricNormal, input.tangent.xyz));
    const float3 bitangent = normalize(cross(geometricNormal, tangent) * input.tangent.w);
    const float3x3 tangentToWorld = float3x3(tangent, bitangent, geometricNormal);

    float3 tangentNormal = gNormalMap.Sample(gMaterialSampler, input.texCoord).xyz * 2.0 - 1.0;
    tangentNormal.y *= gRenderParams.w;
    return normalize(mul(tangentNormal, tangentToWorld));
}

float3 ToneMapAndGammaCorrect(float3 color)
{
    color = 1.0 - exp(-color * gRenderParams.x);
    return pow(saturate(color), 1.0 / 2.2);
}

float4 PSMain(PSInput input) : SV_Target
{
    const float3 albedo = gAlbedoMap.Sample(gMaterialSampler, input.texCoord).rgb;
    const float metallic = saturate(gMetallicMap.Sample(gMaterialSampler, input.texCoord).r);
    const float roughness = clamp(gRoughnessMap.Sample(gMaterialSampler, input.texCoord).r,
                                  0.045, 1.0);
    const float ao = 1.0;
    const float3 normal = GetWorldNormal(input);
    const int debugMode = (int)(gDebugParams.x + 0.5);

    if (debugMode == 1)
        return float4(pow(saturate(albedo), 1.0 / 2.2), 1.0);
    if (debugMode == 2)
        return float4(normal * 0.5 + 0.5, 1.0);
    if (debugMode == 3)
        return float4(metallic.xxx, 1.0);
    if (debugMode == 4)
        return float4(roughness.xxx, 1.0);

    const float3 viewDirection = normalize(gCameraPosition.xyz - input.worldPosition);
    const float nDotV = max(dot(normal, viewDirection), 0.0);
    const float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    float3 directLight = 0.0;
    if (gRenderParams.z > 0.5)
    {
        [unroll]
        for (int i = 0; i < 4; ++i)
        {
            const float3 toLight = gLightPosition[i].xyz - input.worldPosition;
            const float distanceSquared = max(dot(toLight, toLight), 0.01);
            const float3 lightDirection = toLight * rsqrt(distanceSquared);
            const float3 halfwayVector = normalize(viewDirection + lightDirection);
            const float3 radiance = gLightColor[i].rgb / distanceSquared;

            const float distribution = DistributionGGX(normal, halfwayVector, roughness);
            const float geometry = GeometrySmith(normal, viewDirection, lightDirection, roughness);
            const float3 fresnel = FresnelSchlick(max(dot(halfwayVector, viewDirection), 0.0), f0);

            const float3 numerator = distribution * geometry * fresnel;
            const float denominator = max(4.0 * nDotV * max(dot(normal, lightDirection), 0.0),
                                          0.0001);
            const float3 specular = numerator / denominator;

            const float3 kS = fresnel;
            const float3 kD = (1.0 - kS) * (1.0 - metallic);
            const float nDotL = max(dot(normal, lightDirection), 0.0);
            directLight += (kD * albedo / PI + specular) * radiance * nDotL;
        }
    }

    float3 ambient;
    if (gRenderParams.y > 0.5)
    {
        const float3 fresnel = FresnelSchlickRoughness(nDotV, f0, roughness);
        const float3 kS = fresnel;
        const float3 kD = (1.0 - kS) * (1.0 - metallic);

        const float3 irradiance = gIrradianceMap.Sample(gEnvironmentSampler, normal);
        const float3 diffuse = irradiance * albedo;

        const float3 reflection = reflect(-viewDirection, normal);
        const float maxReflectionLod = 11.0;
        const float3 prefilteredColor = gPrefilteredMap.SampleLevel(
            gEnvironmentSampler, reflection, roughness * maxReflectionLod);
        const float2 brdf = gBrdfLut.Sample(
            gEnvironmentSampler, float2(nDotV, roughness));
        const float3 specular = prefilteredColor * (fresnel * brdf.x + brdf.y);

        ambient = (kD * diffuse + specular) * ao;
    }
    else
    {
        ambient = 0.025 * albedo * ao;
    }

    if (debugMode == 5)
    {
        const float nDotL = saturate(dot(normal, normalize(gLightPosition[0].xyz - input.worldPosition)));
        return float4(nDotL.xxx, 1.0);
    }

    return float4(ToneMapAndGammaCorrect(ambient + directLight), 1.0);
}

