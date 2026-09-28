cbuffer SceneConstants : register(b0)
{
    row_major float4x4 gWorld;
    row_major float4x4 gView;
    row_major float4x4 gProjection;
    float4 gCameraPosition;
    float4 gLightPosition[4];
    float4 gLightColor[4];
    float4 gRenderParams;
    float4 gDebugParams;
};

TextureCube<float3> gEnvironmentMap : register(t0);
SamplerState gEnvironmentSampler : register(s0);

struct VSInput
{
    float3 position : POSITION;
};

struct PSInput
{
    float4 position : SV_Position;
    float3 direction : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    row_major float4x4 viewWithoutTranslation = gView;
    viewWithoutTranslation[3][0] = 0.0;
    viewWithoutTranslation[3][1] = 0.0;
    viewWithoutTranslation[3][2] = 0.0;

    float4 clipPosition = mul(mul(float4(input.position, 1.0), viewWithoutTranslation), gProjection);
    output.position = clipPosition.xyww;
    output.direction = input.position;
    return output;
}

float4 PSMain(PSInput input) : SV_Target
{
    float3 color = gEnvironmentMap.SampleLevel(gEnvironmentSampler,
                                               normalize(input.direction), 0.0);
    color = 1.0 - exp(-color * gRenderParams.x);
    color = pow(saturate(color), 1.0 / 2.2);
    return float4(color, 1.0);
}

