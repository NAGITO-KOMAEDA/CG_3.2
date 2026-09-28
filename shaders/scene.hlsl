cbuffer SceneConstants : register(b0)
{
    float4x4 WorldViewProjection;
    float4x4 World;
    float4 LightDirection;
    float4 CameraPosition;
};

cbuffer MaterialConstants : register(b1)
{
    float4 MaterialDiffuse;
};

Texture2D DiffuseTexture : register(t0);
SamplerState LinearSampler : register(s0);

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 texcoord : TEXCOORD;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float2 texcoord : TEXCOORD;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.position = mul(float4(input.position, 1.0f), WorldViewProjection);
    output.normal = normalize(mul(float4(input.normal, 0.0f), World).xyz);
    output.texcoord = input.texcoord;
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    const float3 albedo = DiffuseTexture.Sample(LinearSampler, input.texcoord).rgb * MaterialDiffuse.rgb;
    const float diffuse = saturate(dot(normalize(input.normal), -normalize(LightDirection.xyz)));
    const float lighting = 0.18f + diffuse * 0.82f;
    return float4(albedo * lighting, 1.0f);
}
