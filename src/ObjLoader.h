#pragma once

#include "MathTypes.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct CpuMaterial
{
    std::string name;
    DirectX::XMFLOAT3 diffuse{0.75f, 0.75f, 0.75f};
    std::filesystem::path diffuseTexture;
};

struct MeshPart
{
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    uint32_t materialIndex = 0;
};

struct CpuMesh
{
    std::vector<Vertex> vertices;
    std::vector<CpuMaterial> materials;
    std::vector<MeshPart> parts;
};

CpuMesh LoadObj(const std::filesystem::path& objPath);
