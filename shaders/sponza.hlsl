// Based on the pipeline in slides 32–36 and distance LOD in slide 54 of
// "04 - Normal maps, tesselation.pptx". Input positions are in metres.
cbuffer Frame : register(b0)
{
    float4x4 viewProjection;
    float4 cameraPosition;
    float4 lightDirection;
    float4 tessellation; // x=maximum, y=near distance, z=far distance, w=enabled
    float4 display;      // x=wireframe, y=show tess levels, z=animation seconds
};

cbuffer Material : register(b1)
{
    float4 material;     // x=displacement strength, y=has normal map, z=water, w=opacity
};

Texture2D diffuseMap : register(t0);
Texture2D normalMap : register(t1);
Texture2D heightMap : register(t2);
SamplerState textureSampler : register(s0);

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD0;
};

struct ControlPoint
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD0;
};

struct PixelInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 tangent : TEXCOORD2;
    float2 uv : TEXCOORD3;
    float tessLevel : TEXCOORD4;
};

ControlPoint VSControl(VertexInput input)
{
    ControlPoint result;
    result.position = input.position;
    result.normal = input.normal;
    result.tangent = input.tangent;
    result.uv = input.uv;
    return result;
}

PixelInput VSPlain(VertexInput input)
{
    PixelInput result;
    result.worldPosition = input.position;
    result.position = mul(float4(input.position, 1.0), viewProjection);
    result.normal = input.normal;
    result.tangent = input.tangent;
    result.uv = input.uv;
    result.tessLevel = 1.0;
    return result;
}

struct PatchData
{
    float edges[3] : SV_TessFactor;
    float inside : SV_InsideTessFactor;
};

// Height and analytic derivatives of a continuous world-space wave field.
// Shared patch boundaries evaluate exactly the same displacement.
float3 WaterWave(float2 p)
{
    float3 wave = 0.0;
    const float2 directions[3] = {float2(1,0), float2(0.6,0.8), float2(-0.8,0.6)};
    const float amplitudes[3] = {0.065, 0.035, 0.018};
    const float frequencies[3] = {2.4, 4.1, 7.3};
    const float speeds[3] = {1.3, 1.8, 2.5};
    [unroll] for (int j=0; j<3; ++j)
    {
        float phase = dot(p,directions[j])*frequencies[j] - display.z*speeds[j];
        wave.x += amplitudes[j]*sin(phase);
        wave.yz += amplitudes[j]*frequencies[j]*cos(phase)*directions[j];
    }
    return wave;
}

float EdgeFactor(float3 a, float3 b)
{
    if (tessellation.w < 0.5 || material.x <= 0.0) return 1.0;
    float distanceToEye = distance((a + b) * 0.5, cameraPosition.xyz);
    float t = saturate((tessellation.z - distanceToEye) /
                       max(tessellation.z - tessellation.y, 0.001));
    return lerp(1.0, tessellation.x, t);
}

PatchData PatchFactors(InputPatch<ControlPoint, 3> patch)
{
    PatchData result;
    // Each factor depends on the two endpoints of that edge. Adjacent
    // triangles therefore receive the same outer factor on a shared edge.
    result.edges[0] = EdgeFactor(patch[1].position, patch[2].position);
    result.edges[1] = EdgeFactor(patch[2].position, patch[0].position);
    result.edges[2] = EdgeFactor(patch[0].position, patch[1].position);
    result.inside = (result.edges[0] + result.edges[1] + result.edges[2]) / 3.0;
    return result;
}

[domain("tri")]
[partitioning("fractional_even")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("PatchFactors")]
ControlPoint HSMain(InputPatch<ControlPoint, 3> patch,
                    uint id : SV_OutputControlPointID)
{
    return patch[id];
}

[domain("tri")]
PixelInput DSMain(PatchData factors, float3 barycentric : SV_DomainLocation,
                  const OutputPatch<ControlPoint, 3> patch)
{
    PixelInput result;
    float3 position = 0.0;
    float3 normal = 0.0;
    float3 tangent = 0.0;
    float2 uv = 0.0;
    [unroll] for (int i = 0; i < 3; ++i)
    {
        position += patch[i].position * barycentric[i];
        normal += patch[i].normal * barycentric[i];
        tangent += patch[i].tangent.xyz * barycentric[i];
        uv += patch[i].uv * barycentric[i];
    }
    normal = normalize(normal);
    if (material.z > 0.5)
    {
        float3 wave = WaterWave(position.xz);
        position.y += wave.x;
        normal = normalize(float3(-wave.y, 1.0, -wave.z));
        tangent = normalize(float3(1.0, wave.y, 0.0));
    }
    else if (tessellation.w > 0.5 && material.x > 0.0)
    {
        // Explicit LOD is required in the domain shader: implicit derivatives
        // are unavailable at this stage of the pipeline.
        // A prefiltered, camera-independent level prevents tiny height-map
        // features from jumping between newly generated tessellation vertices.
        float height = heightMap.SampleLevel(textureSampler, uv, 4).r;
        position += normal * ((height - 0.5) * material.x);
    }
    result.worldPosition = position;
    result.position = mul(float4(position, 1.0), viewProjection);
    result.normal = normal;
    result.tangent = float4(normalize(tangent), patch[0].tangent.w);
    result.uv = uv;
    result.tessLevel = factors.inside;
    return result;
}

float4 PSMain(PixelInput input) : SV_TARGET
{
    if (material.z > 0.5)
    {
        float3 wave = WaterWave(input.worldPosition.xz);
        float3 N = normalize(float3(-wave.y, 1.0, -wave.z));
        float3 V = normalize(cameraPosition.xyz-input.worldPosition);
        if (dot(N,V)<0.0) N=-N;
        float3 L = normalize(-lightDirection.xyz);
        float fresnel = 0.02 + 0.98*pow(1.0-saturate(dot(N,V)),5.0);
        float highlight = pow(saturate(dot(N,normalize(L+V))),160.0);
        float3 color = lerp(float3(0.025,0.22,0.27),float3(0.38,0.58,0.72),fresnel);
        color += highlight*0.8;
        if (display.y > 0.5)
        {
            float t=saturate((input.tessLevel-1.0)/max(tessellation.x-1.0,1.0));
            color=lerp(float3(0.1,0.25,0.95),float3(1.0,0.2,0.1),t);
        }
        return float4(pow(saturate(color),1.0/2.2),
                      saturate(material.w+0.35*fresnel+0.15*highlight));
    }
    float4 albedo = diffuseMap.Sample(textureSampler, input.uv);
    clip(albedo.a - 0.35);

    float3 N = normalize(input.normal);
    if (material.y > 0.5)
    {
        float3 T = normalize(input.tangent.xyz - N * dot(input.tangent.xyz, N));
        float3 B = normalize(cross(N, T)) * input.tangent.w;
        float3 mapped = normalMap.Sample(textureSampler, input.uv).xyz * 2.0 - 1.0;
        N = normalize(mapped.x * T + mapped.y * B + mapped.z * N);
    }

    float3 L = normalize(-lightDirection.xyz);
    float diffuse = saturate(dot(N, L));
    float3 V = normalize(cameraPosition.xyz - input.worldPosition);
    float3 H = normalize(L + V);
    float specular = pow(saturate(dot(N, H)), 32.0) * 0.12;
    float3 color = albedo.rgb * (0.35 + 0.65 * diffuse) + specular;
    if (display.y > 0.5 && material.x > 0.0)
    {
        float t = saturate((input.tessLevel - 1.0) / max(tessellation.x - 1.0, 1.0));
        color = lerp(float3(0.1, 0.25, 0.95), float3(1.0, 0.2, 0.1), t);
    }
    // The swap-chain view is UNORM, so encode linear lighting for the display.
    return float4(pow(saturate(color), 1.0 / 2.2), 1.0);
}
