#include "ObjLoader.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

using namespace DirectX;

namespace
{
struct ObjIndex
{
    int position = 0;
    int texcoord = 0;
    int normal = 0;
};

int ResolveIndex(int index, size_t count)
{
    if (index > 0)
        return index - 1;
    if (index < 0)
        return static_cast<int>(count) + index;
    return -1;
}

ObjIndex ParseIndex(std::string_view token)
{
    ObjIndex result{};
    std::array<int*, 3> values{&result.position, &result.texcoord, &result.normal};
    size_t begin = 0;
    for (size_t component = 0; component < values.size(); ++component)
    {
        const size_t end = token.find('/', begin);
        const std::string_view part = token.substr(begin, end == std::string_view::npos ? token.size() - begin : end - begin);
        if (!part.empty())
            std::from_chars(part.data(), part.data() + part.size(), *values[component]);
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    return result;
}

std::vector<CpuMaterial> LoadMaterials(const std::filesystem::path& path)
{
    std::ifstream file(path);
    if (!file)
        throw std::runtime_error("Cannot open material library: " + path.string());

    std::vector<CpuMaterial> materials;
    CpuMaterial* current = nullptr;
    std::string line;
    while (std::getline(file, line))
    {
        std::istringstream stream(line);
        std::string command;
        stream >> command;
        if (command == "newmtl")
        {
            materials.emplace_back();
            current = &materials.back();
            stream >> current->name;
        }
        else if (current && command == "Kd")
        {
            stream >> current->diffuse.x >> current->diffuse.y >> current->diffuse.z;
        }
        else if (current && command == "map_Kd")
        {
            std::string texture;
            std::getline(stream >> std::ws, texture);
            current->diffuseTexture = path.parent_path() / std::filesystem::path(texture);
        }
    }
    return materials;
}

Vertex MakeVertex(const ObjIndex& index,
                  const std::vector<XMFLOAT3>& positions,
                  const std::vector<XMFLOAT3>& normals,
                  const std::vector<XMFLOAT2>& texcoords)
{
    Vertex vertex{};
    const int p = ResolveIndex(index.position, positions.size());
    const int n = ResolveIndex(index.normal, normals.size());
    const int t = ResolveIndex(index.texcoord, texcoords.size());
    if (p < 0 || static_cast<size_t>(p) >= positions.size())
        throw std::runtime_error("OBJ face references an invalid position");
    vertex.position = positions[static_cast<size_t>(p)];
    if (n >= 0 && static_cast<size_t>(n) < normals.size())
        vertex.normal = normals[static_cast<size_t>(n)];
    if (t >= 0 && static_cast<size_t>(t) < texcoords.size())
        vertex.texcoord = texcoords[static_cast<size_t>(t)];
    return vertex;
}

void GenerateFaceNormal(Vertex& a, Vertex& b, Vertex& c)
{
    const XMVECTOR p0 = XMLoadFloat3(&a.position);
    const XMVECTOR p1 = XMLoadFloat3(&b.position);
    const XMVECTOR p2 = XMLoadFloat3(&c.position);
    const XMVECTOR normal = XMVector3Normalize(XMVector3Cross(p1 - p0, p2 - p0));
    XMStoreFloat3(&a.normal, normal);
    XMStoreFloat3(&b.normal, normal);
    XMStoreFloat3(&c.normal, normal);
}
}

CpuMesh LoadObj(const std::filesystem::path& objPath)
{
    std::ifstream file(objPath);
    if (!file)
        throw std::runtime_error("Cannot open OBJ: " + objPath.string());

    CpuMesh result;
    std::vector<XMFLOAT3> positions;
    std::vector<XMFLOAT3> normals;
    std::vector<XMFLOAT2> texcoords;
    std::unordered_map<std::string, std::vector<Vertex>> verticesByMaterial;
    std::vector<std::string> materialOrder;
    std::string currentMaterial = "__default";
    verticesByMaterial[currentMaterial] = {};
    materialOrder.push_back(currentMaterial);

    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#')
            continue;

        std::istringstream stream(line);
        std::string command;
        stream >> command;
        if (command == "v")
        {
            XMFLOAT3 value{};
            stream >> value.x >> value.y >> value.z;
            positions.push_back(value);
        }
        else if (command == "vn")
        {
            XMFLOAT3 value{};
            stream >> value.x >> value.y >> value.z;
            normals.push_back(value);
        }
        else if (command == "vt")
        {
            XMFLOAT2 value{};
            stream >> value.x >> value.y;
            value.y = 1.0f - value.y;
            texcoords.push_back(value);
        }
        else if (command == "mtllib")
        {
            std::string filename;
            std::getline(stream >> std::ws, filename);
            result.materials = LoadMaterials(objPath.parent_path() / filename);
        }
        else if (command == "usemtl")
        {
            stream >> currentMaterial;
            if (!verticesByMaterial.contains(currentMaterial))
            {
                verticesByMaterial[currentMaterial] = {};
                materialOrder.push_back(currentMaterial);
            }
        }
        else if (command == "f")
        {
            std::vector<ObjIndex> polygon;
            std::string token;
            while (stream >> token)
                polygon.push_back(ParseIndex(token));
            if (polygon.size() < 3)
                continue;

            auto& output = verticesByMaterial[currentMaterial];
            for (size_t i = 1; i + 1 < polygon.size(); ++i)
            {
                Vertex a = MakeVertex(polygon[0], positions, normals, texcoords);
                Vertex b = MakeVertex(polygon[i], positions, normals, texcoords);
                Vertex c = MakeVertex(polygon[i + 1], positions, normals, texcoords);
                if (polygon[0].normal == 0 || polygon[i].normal == 0 || polygon[i + 1].normal == 0)
                    GenerateFaceNormal(a, b, c);
                output.push_back(a);
                output.push_back(b);
                output.push_back(c);
            }
        }
    }

    std::unordered_map<std::string, uint32_t> materialIndices;
    for (uint32_t i = 0; i < result.materials.size(); ++i)
        materialIndices[result.materials[i].name] = i;
    if (!materialIndices.contains("__default"))
    {
        materialIndices["__default"] = static_cast<uint32_t>(result.materials.size());
        result.materials.push_back(CpuMaterial{"__default"});
    }

    for (const std::string& material : materialOrder)
    {
        auto& vertices = verticesByMaterial[material];
        if (vertices.empty())
            continue;
        MeshPart part;
        part.firstVertex = static_cast<uint32_t>(result.vertices.size());
        part.vertexCount = static_cast<uint32_t>(vertices.size());
        part.materialIndex = materialIndices.contains(material) ? materialIndices[material] : materialIndices["__default"];
        result.vertices.insert(result.vertices.end(), vertices.begin(), vertices.end());
        result.parts.push_back(part);
    }

    if (result.vertices.empty())
        throw std::runtime_error("OBJ contains no triangles: " + objPath.string());
    return result;
}
