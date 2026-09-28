#pragma once

#include <DirectXMath.h>

struct Vertex
{
    DirectX::XMFLOAT3 position{};
    DirectX::XMFLOAT3 normal{};
    DirectX::XMFLOAT2 texcoord{};
};

struct Particle
{
    DirectX::XMFLOAT3 position{};
    float age = 0.0f;
    DirectX::XMFLOAT3 velocity{};
    float lifetime = 1.0f;
    DirectX::XMFLOAT4 color{1.0f, 1.0f, 1.0f, 1.0f};
    float size = 0.1f;
    DirectX::XMFLOAT3 padding{};
};

static_assert(sizeof(Particle) == 64);
