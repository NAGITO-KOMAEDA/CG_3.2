cbuffer LightingConstants : register(b0)
{
    float4x4 InverseViewProjection;
    float3 CameraPosition;
    float LightingPadding0;
    float2 ScreenSize;
    float2 InverseScreenSize;
};

cbuffer LightConstants : register(b1)
{
    float4 LightColorIntensity;
    float4 LightPositionRange;
    float4 LightDirectionInnerCos;
    float4 LightParameters;
};

Texture2D<float4> AlbedoSpecularTexture : register(t0);
Texture2D<float4> NormalShininessTexture : register(t1);
Texture2D<float> DepthTexture : register(t2);

struct FullScreenOutput
{
    float4 position : SV_POSITION;
};

FullScreenOutput VSMain(uint vertexId : SV_VertexID)
{
    FullScreenOutput output;
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float3 ReconstructWorldPosition(uint2 pixel, float depth)
{
    const float2 uv = (float2(pixel) + 0.5) * InverseScreenSize;
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 world = mul(float4(ndc, depth, 1.0), InverseViewProjection);
    return world.xyz / world.w;
}

float3 EvaluateBlinnPhong(
    float3 albedo,
    float specularStrength,
    float shininess,
    float3 normal,
    float3 viewDirection,
    float3 lightDirection,
    float3 radiance)
{
    const float nDotL = saturate(dot(normal, lightDirection));
    const float3 halfVector = normalize(lightDirection + viewDirection);
    const float specular = pow(saturate(dot(normal, halfVector)), shininess) * specularStrength;
    return (albedo * nDotL + specular.xxx) * radiance;
}

float4 PSMain(FullScreenOutput input) : SV_Target
{
    const uint2 pixel = uint2(input.position.xy);
    const float depth = DepthTexture.Load(int3(pixel, 0));
    if (depth >= 0.999999)
    {
        discard;
    }

    const float4 albedoSpecular = AlbedoSpecularTexture.Load(int3(pixel, 0));
    const float4 normalShininess = NormalShininessTexture.Load(int3(pixel, 0));
    const float3 worldPosition = ReconstructWorldPosition(pixel, depth);
    const float3 normal = normalize(normalShininess.xyz * 2.0 - 1.0);
    const float shininess = max(4.0, normalShininess.w * 256.0);
    const float3 viewDirection = normalize(CameraPosition - worldPosition);
    const float3 lightColor = LightColorIntensity.rgb * LightColorIntensity.w;
    const uint lightType = (uint)LightParameters.y;

    if (lightType == 0)
    {
        const float3 lightDirection = normalize(-LightDirectionInnerCos.xyz);
        const float3 ambient = albedoSpecular.rgb * LightParameters.z;
        const float3 directional = EvaluateBlinnPhong(
            albedoSpecular.rgb,
            albedoSpecular.a,
            shininess,
            normal,
            viewDirection,
            lightDirection,
            lightColor);
        return float4(ambient + directional, 1.0);
    }

    const float3 toLight = LightPositionRange.xyz - worldPosition;
    const float distanceToLight = length(toLight);
    const float range = LightPositionRange.w;
    if (distanceToLight >= range)
    {
        discard;
    }

    const float3 lightDirection = toLight / max(distanceToLight, 0.0001);
    float attenuation = saturate(1.0 - distanceToLight / range);
    attenuation *= attenuation;

    if (lightType == 2)
    {
        const float3 lightToPixel = -lightDirection;
        const float coneCosine = dot(lightToPixel, normalize(LightDirectionInnerCos.xyz));
        const float spot = smoothstep(LightParameters.x, LightDirectionInnerCos.w, coneCosine);
        attenuation *= spot;
        if (attenuation <= 0.0001)
        {
            discard;
        }
    }

    const float3 result = EvaluateBlinnPhong(
        albedoSpecular.rgb,
        albedoSpecular.a,
        shininess,
        normal,
        viewDirection,
        lightDirection,
        lightColor * attenuation);
    return float4(result, 0.0);
}
