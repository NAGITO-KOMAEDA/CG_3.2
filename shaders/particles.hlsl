struct Particle
{
    float3 position;
    float age;
    float3 velocity;
    float lifetime;
    float4 color;
    float size;
    float3 padding;
};

cbuffer ParticleConstants : register(b0)
{
    float4x4 ViewProjection;
    float4 CameraRight;
    float4 CameraUp;
    float4 EmitterAndTime;
    float4 GravityAndDelta;
    uint ParticleCount;
    uint FrameIndex;
    float2 ConstantsPadding;
};

StructuredBuffer<Particle> RenderParticles : register(t0);
ConsumeStructuredBuffer<Particle> InputParticles : register(u0);
AppendStructuredBuffer<Particle> OutputParticles : register(u1);

uint Hash(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

float Random01(inout uint state)
{
    state = Hash(state);
    return (state & 0x00ffffffu) / 16777216.0f;
}

[numthreads(256, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= ParticleCount)
        return;

    Particle particle = InputParticles.Consume();
    const float deltaTime = GravityAndDelta.w;
    particle.age += deltaTime;

    if (particle.age >= particle.lifetime || particle.position.y < -0.05f)
    {
        uint randomState = Hash(dispatchThreadId.x + FrameIndex * 1664525u);
        const float angle = Random01(randomState) * 6.283185307f;
        const float radius = sqrt(Random01(randomState)) * 0.35f;
        const float speed = lerp(2.5f, 5.5f, Random01(randomState));
        particle.position = EmitterAndTime.xyz + float3(cos(angle) * radius, 0.0f, sin(angle) * radius);
        particle.velocity = float3(cos(angle) * 0.45f, speed, sin(angle) * 0.45f);
        particle.age = 0.0f;
        particle.lifetime = lerp(2.0f, 5.0f, Random01(randomState));
        particle.color = float4(1.0f, lerp(0.35f, 0.85f, Random01(randomState)), 0.08f, 1.0f);
        particle.size = lerp(0.055f, 0.10f, Random01(randomState));
    }
    else
    {
        const float3 wind = float3(sin(EmitterAndTime.w * 0.9f + particle.position.y) * 0.25f, 0.0f,
                                   cos(EmitterAndTime.w * 0.7f + particle.position.x) * 0.18f);
        particle.velocity += (GravityAndDelta.xyz + wind) * deltaTime;
        particle.position += particle.velocity * deltaTime;
    }

    const float life = saturate(particle.age / particle.lifetime);
    particle.color.rgb = lerp(float3(1.0f, 0.85f, 0.12f), float3(0.95f, 0.12f, 0.02f), life);
    OutputParticles.Append(particle);
}

struct VSOutput
{
    float3 position : POSITION;
    float4 color : COLOR;
    float size : PSIZE;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    const Particle particle = RenderParticles[vertexId];
    VSOutput output;
    output.position = particle.position;
    output.color = particle.color;
    output.size = particle.size;
    return output;
}

struct GSOutput
{
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD;
    float4 color : COLOR;
};

[maxvertexcount(4)]
void GSMain(point VSOutput input[1], inout TriangleStream<GSOutput> stream)
{
    const float2 corners[4] = {
        float2(-1.0f,  1.0f),
        float2( 1.0f,  1.0f),
        float2(-1.0f, -1.0f),
        float2( 1.0f, -1.0f)
    };
    const float2 texcoords[4] = {
        float2(0.0f, 0.0f),
        float2(1.0f, 0.0f),
        float2(0.0f, 1.0f),
        float2(1.0f, 1.0f)
    };

    [unroll]
    for (uint i = 0; i < 4; ++i)
    {
        const float3 worldPosition = input[0].position
            + CameraRight.xyz * corners[i].x * input[0].size
            + CameraUp.xyz * corners[i].y * input[0].size;
        GSOutput output;
        output.position = mul(float4(worldPosition, 1.0f), ViewProjection);
        output.texcoord = texcoords[i];
        output.color = input[0].color;
        stream.Append(output);
    }
}

float4 PSMain(GSOutput input) : SV_TARGET
{
    const float2 centered = input.texcoord * 2.0f - 1.0f;
    const float radiusSquared = dot(centered, centered);
    clip(1.0f - radiusSquared);
    const float highlight = saturate(1.15f - radiusSquared * 0.65f);
    return float4(input.color.rgb * highlight, 1.0f);
}
