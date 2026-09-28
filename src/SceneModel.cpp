#include "SceneModel.h"
#include "TextureLoader.h"

#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <unordered_map>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace
{
    void ThrowIfFailed(HRESULT hr, const char* message)
    {
        if (FAILED(hr))
            throw std::runtime_error(message);
    }

    std::filesystem::path MaterialTexturePath(
        const aiMaterial* material,
        const std::filesystem::path& modelDirectory,
        std::initializer_list<aiTextureType> types)
    {
        for (const aiTextureType type : types)
        {
            aiString value;
            if (material->GetTextureCount(type) > 0 &&
                material->GetTexture(type, 0, &value) == AI_SUCCESS)
            {
                std::filesystem::path relative(value.C_Str());
                return (modelDirectory / relative).lexically_normal();
            }
        }
        return {};
    }
}

void SceneModel::Load(ID3D11Device* device,
                      ID3D11DeviceContext* context,
                      const std::filesystem::path& path)
{
    materials_.clear();
    meshes_.clear();

    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(
        path.string(),
        aiProcess_Triangulate |
        aiProcess_JoinIdenticalVertices |
        aiProcess_GenSmoothNormals |
        aiProcess_CalcTangentSpace |
        aiProcess_ImproveCacheLocality |
        aiProcess_PreTransformVertices |
        aiProcess_SortByPType |
        aiProcess_ConvertToLeftHanded);
    if (!scene || !scene->HasMeshes())
        throw std::runtime_error(std::string("Assimp cannot load Sponza: ") + importer.GetErrorString());

    const auto white = TextureLoader::CreateSolid(device, XMFLOAT4(1, 1, 1, 1), true);
    const auto flatNormal = TextureLoader::CreateSolid(device, XMFLOAT4(0.5f, 0.5f, 1, 1), false);
    const std::filesystem::path modelDirectory = path.parent_path();
    std::unordered_map<std::wstring, ComPtr<ID3D11ShaderResourceView>> albedoCache;
    std::unordered_map<std::wstring, ComPtr<ID3D11ShaderResourceView>> normalCache;

    materials_.resize(std::max(1u, scene->mNumMaterials));
    for (unsigned int i = 0; i < scene->mNumMaterials; ++i)
    {
        const aiMaterial* source = scene->mMaterials[i];
        Material material{white, flatNormal};

        const auto albedoPath = MaterialTexturePath(
            source, modelDirectory, {aiTextureType_DIFFUSE, aiTextureType_BASE_COLOR});
        if (!albedoPath.empty() && std::filesystem::exists(albedoPath))
        {
            const auto key = albedoPath.wstring();
            auto found = albedoCache.find(key);
            if (found == albedoCache.end())
                found = albedoCache.emplace(
                    key, TextureLoader::LoadTGA(device, context, albedoPath, true)).first;
            material.albedo = found->second;
        }

        const auto normalPath = MaterialTexturePath(
            source, modelDirectory,
            {aiTextureType_NORMALS, aiTextureType_HEIGHT, aiTextureType_DISPLACEMENT});
        if (!normalPath.empty() && std::filesystem::exists(normalPath))
        {
            const auto key = normalPath.wstring();
            auto found = normalCache.find(key);
            if (found == normalCache.end())
                found = normalCache.emplace(
                    key, TextureLoader::LoadTGA(device, context, normalPath, false)).first;
            material.normal = found->second;
        }
        materials_[i] = std::move(material);
    }

    XMFLOAT3 minimum{FLT_MAX, FLT_MAX, FLT_MAX};
    XMFLOAT3 maximum{-FLT_MAX, -FLT_MAX, -FLT_MAX};

    for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex)
    {
        const aiMesh* source = scene->mMeshes[meshIndex];
        if (!source->HasPositions() || source->mNumFaces == 0)
            continue;

        std::vector<SceneVertex> vertices;
        vertices.reserve(source->mNumVertices);
        for (unsigned int i = 0; i < source->mNumVertices; ++i)
        {
            const aiVector3D& p = source->mVertices[i];
            const aiVector3D n = source->HasNormals() ? source->mNormals[i] : aiVector3D(0, 1, 0);
            const aiVector3D uv = source->HasTextureCoords(0) ? source->mTextureCoords[0][i]
                                                              : aiVector3D(0, 0, 0);
            aiVector3D t(1, 0, 0), b(0, 0, 1);
            if (source->HasTangentsAndBitangents())
            {
                t = source->mTangents[i];
                b = source->mBitangents[i];
            }
            const float handedness = ((n ^ t) * b) < 0.0f ? -1.0f : 1.0f;
            vertices.push_back({
                XMFLOAT3(p.x, p.y, p.z), XMFLOAT3(n.x, n.y, n.z),
                XMFLOAT2(uv.x, uv.y), XMFLOAT4(t.x, t.y, t.z, handedness)});
            minimum.x = std::min(minimum.x, p.x);
            minimum.y = std::min(minimum.y, p.y);
            minimum.z = std::min(minimum.z, p.z);
            maximum.x = std::max(maximum.x, p.x);
            maximum.y = std::max(maximum.y, p.y);
            maximum.z = std::max(maximum.z, p.z);
        }

        std::vector<std::uint32_t> indices;
        indices.reserve(static_cast<std::size_t>(source->mNumFaces) * 3u);
        for (unsigned int i = 0; i < source->mNumFaces; ++i)
        {
            const aiFace& face = source->mFaces[i];
            if (face.mNumIndices == 3)
                indices.insert(indices.end(), {face.mIndices[0], face.mIndices[1], face.mIndices[2]});
        }
        if (vertices.empty() || indices.empty())
            continue;

        D3D11_BUFFER_DESC vbDesc{};
        vbDesc.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(SceneVertex));
        vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
        vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vbData{vertices.data()};

        D3D11_BUFFER_DESC ibDesc{};
        ibDesc.ByteWidth = static_cast<UINT>(indices.size() * sizeof(std::uint32_t));
        ibDesc.Usage = D3D11_USAGE_IMMUTABLE;
        ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA ibData{indices.data()};

        Mesh mesh;
        ThrowIfFailed(device->CreateBuffer(&vbDesc, &vbData, &mesh.vertexBuffer),
                      "Cannot create Sponza vertex buffer.");
        ThrowIfFailed(device->CreateBuffer(&ibDesc, &ibData, &mesh.indexBuffer),
                      "Cannot create Sponza index buffer.");
        mesh.indexCount = static_cast<UINT>(indices.size());
        mesh.materialIndex = std::min(source->mMaterialIndex,
                                      static_cast<unsigned int>(materials_.size() - 1u));
        meshes_.push_back(std::move(mesh));
    }

    if (meshes_.empty())
        throw std::runtime_error("Sponza contains no drawable triangle meshes.");

    center_ = XMFLOAT3((minimum.x + maximum.x) * 0.5f,
                       (minimum.y + maximum.y) * 0.5f,
                       (minimum.z + maximum.z) * 0.5f);
    radius_ = std::max({maximum.x - minimum.x, maximum.y - minimum.y,
                        maximum.z - minimum.z}) * 0.5f;
    radius_ = std::max(radius_, 0.0001f);
}

void SceneModel::Draw(ID3D11DeviceContext* context) const
{
    const UINT stride = sizeof(SceneVertex);
    const UINT offset = 0;
    for (const Mesh& mesh : meshes_)
    {
        const Material& material = materials_[mesh.materialIndex];
        ID3D11ShaderResourceView* resources[] = {material.albedo.Get(), material.normal.Get()};
        context->PSSetShaderResources(0, 2, resources);
        ID3D11Buffer* vertexBuffer = mesh.vertexBuffer.Get();
        context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
        context->IASetIndexBuffer(mesh.indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
        context->DrawIndexed(mesh.indexCount, 0, 0);
    }
}

XMMATRIX SceneModel::NormalizationTransform() const
{
    const float scale = 2.0f / radius_;
    return XMMatrixTranslation(-center_.x, -center_.y, -center_.z) *
           XMMatrixScaling(scale, scale, scale);
}
