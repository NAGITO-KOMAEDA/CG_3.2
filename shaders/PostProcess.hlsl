cbuffer PostConstants : register(b0)
{
    float4 gTexelAndDirection; // inverse width, inverse height, direction x, direction y
    float4 gBloomParams;       // threshold, soft knee, intensity, enabled
    float4 gDofParams;         // focus distance, focus range, max radius in pixels, enabled
    float4 gCameraParams;      // near plane, far plane, exposure, debug mode
};

Texture2D<float4> gSource : register(t0);
Texture2D<float4> gBloom : register(t1);
Texture2D<float> gDepth : register(t2);
SamplerState gLinearClamp : register(s0);

struct FullScreenOutput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

FullScreenOutput VSFullScreen(uint vertexId : SV_VertexID)
{
    FullScreenOutput output;
    output.texCoord = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.texCoord * float2(2.0, -2.0) + float2(-1.0, 1.0),
                             0.0, 1.0);
    return output;
}

float4 PSBright(FullScreenOutput input) : SV_Target
{
    const float3 color = gSource.SampleLevel(gLinearClamp, input.texCoord, 0).rgb;
    const float brightness = max(color.r, max(color.g, color.b));
    const float threshold = gBloomParams.x;
    const float knee = max(gBloomParams.y, 0.0001);
    float soft = clamp(brightness - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 0.0001);
    const float contribution = max(brightness - threshold, soft) /
                               max(brightness, 0.0001);
    return float4(color * contribution, 1.0);
}

float4 PSBlur(FullScreenOutput input) : SV_Target
{
    static const float weights[5] = {
        0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162};
    const float2 step = gTexelAndDirection.xy * gTexelAndDirection.zw;
    float3 color = gSource.SampleLevel(gLinearClamp, input.texCoord, 0).rgb * weights[0];
    [unroll]
    for (int i = 1; i < 5; ++i)
    {
        color += gSource.SampleLevel(gLinearClamp, input.texCoord + step * i, 0).rgb * weights[i];
        color += gSource.SampleLevel(gLinearClamp, input.texCoord - step * i, 0).rgb * weights[i];
    }
    return float4(color, 1.0);
}

float LinearizeDepth(float depth)
{
    const float nearPlane = gCameraParams.x;
    const float farPlane = gCameraParams.y;
    return nearPlane * farPlane /
           max(farPlane - depth * (farPlane - nearPlane), 0.0001);
}

float CircleOfConfusion(float viewDepth)
{
    return saturate(abs(viewDepth - gDofParams.x) / max(gDofParams.y, 0.001));
}

float3 DepthOfField(float2 uv, float coc)
{
    static const float2 poisson[12] = {
        float2(-0.326, -0.406), float2(-0.840, -0.074),
        float2(-0.696,  0.457), float2(-0.203,  0.621),
        float2( 0.962, -0.195), float2( 0.473, -0.480),
        float2( 0.519,  0.767), float2( 0.185, -0.893),
        float2( 0.507,  0.064), float2( 0.896,  0.412),
        float2(-0.322, -0.933), float2(-0.792, -0.598)};

    const float2 radius = gTexelAndDirection.xy * gDofParams.z * coc;
    float3 color = gSource.SampleLevel(gLinearClamp, uv, 0).rgb;
    float totalWeight = 1.0;
    [unroll]
    for (int i = 0; i < 12; ++i)
    {
        const float2 sampleUv = uv + poisson[i] * radius;
        const float sampleDepth = LinearizeDepth(
            gDepth.SampleLevel(gLinearClamp, sampleUv, 0));
        const float sampleCoc = CircleOfConfusion(sampleDepth);
        const float weight = saturate(sampleCoc * 2.0 + 0.15);
        color += gSource.SampleLevel(gLinearClamp, sampleUv, 0).rgb * weight;
        totalWeight += weight;
    }
    return color / totalWeight;
}

float3 ToneMapAndGammaCorrect(float3 hdr)
{
    const float3 mapped = 1.0 - exp(-hdr * gCameraParams.z);
    return pow(saturate(mapped), 1.0 / 2.2);
}

float4 PSFinal(FullScreenOutput input) : SV_Target
{
    const float2 uv = input.texCoord;
    const float3 sharp = gSource.SampleLevel(gLinearClamp, uv, 0).rgb;
    const float viewDepth = LinearizeDepth(gDepth.SampleLevel(gLinearClamp, uv, 0));
    const float coc = CircleOfConfusion(viewDepth);
    const int debugMode = (int)(gCameraParams.w + 0.5);

    if (debugMode == 2)
        return float4(ToneMapAndGammaCorrect(gBloom.SampleLevel(gLinearClamp, uv, 0).rgb), 1.0);
    if (debugMode == 3)
        return float4(saturate(viewDepth / gCameraParams.y).xxx, 1.0);
    if (debugMode == 4)
        return float4(coc.xxx, 1.0);

    float3 sceneColor = sharp;
    if (gDofParams.w > 0.5 && debugMode == 0)
    {
        const float3 blurred = DepthOfField(uv, coc);
        sceneColor = lerp(sharp, blurred, smoothstep(0.05, 1.0, coc));
    }

    if (gBloomParams.w > 0.5 && debugMode == 0)
        sceneColor += gBloom.SampleLevel(gLinearClamp, uv, 0).rgb * gBloomParams.z;

    return float4(ToneMapAndGammaCorrect(sceneColor), 1.0);
}

