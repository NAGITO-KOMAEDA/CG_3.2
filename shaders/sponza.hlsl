// Based on the pipeline in slides 32–36 and distance LOD in slide 54 of
// "04 - Normal maps, tesselation.pptx". Input positions are in metres.
cbuffer Frame : register(b0)
{
    float4x4 viewProjection;
    float4 cameraPosition;
    float4 lightDirection;
    float4 tessellation; // x=maximum, y=near distance, z=far distance, w=enabled
    float4 display;      // x=wireframe (rasterizer), y=show tess levels
};

cbuffer Material : register(b1)
{
    float4 material;     // x=displacement strength, y=has normal map
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
    if (tessellation.w > 0.5 && material.x > 0.0)
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
