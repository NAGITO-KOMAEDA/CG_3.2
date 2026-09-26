cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProjection;
};

cbuffer MaterialConstants : register(b1)
{
    float4 MaterialBaseColor;
    float MaterialSpecular;
    float MaterialShininess;
    float2 MaterialPadding;
};

Texture2D<float4> DiffuseTexture : register(t0);
SamplerState LinearWrapSampler : register(s0);

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct PixelInput
{
    float4 position : SV_POSITION;
    float3 worldNormal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct GBufferOutput
{
    float4 albedoSpecular : SV_Target0;
    float4 normalShininess : SV_Target1;
};

PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    output.position = mul(float4(input.position, 1.0), ViewProjection);
    output.worldNormal = input.normal;
    output.uv = input.uv;
    return output;
}

[earlydepthstencil]
GBufferOutput PSMain(PixelInput input)
{
    const float4 textureColor = DiffuseTexture.Sample(LinearWrapSampler, input.uv);
    clip(textureColor.a - 0.10);

    GBufferOutput output;
    output.albedoSpecular = float4(textureColor.rgb * MaterialBaseColor.rgb, MaterialSpecular);
    output.normalShininess = float4(normalize(input.worldNormal) * 0.5 + 0.5, MaterialShininess / 256.0);
    return output;
}
