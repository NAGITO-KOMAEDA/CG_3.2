#include "Model.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace
{
    void ThrowIfFailed(HRESULT hr, const char* message)
    {
        if (FAILED(hr))
        {
            throw std::runtime_error(message);
        }
    }
}

void Model::Load(ID3D11Device* device, const std::filesystem::path& path)
{
    meshes_.clear();

    Assimp::Importer importer;
    const unsigned int flags =
        aiProcess_Triangulate |
        aiProcess_JoinIdenticalVertices |
        aiProcess_GenSmoothNormals |
        aiProcess_CalcTangentSpace |
        aiProcess_ImproveCacheLocality |
        aiProcess_PreTransformVertices |
        aiProcess_SortByPType |
        aiProcess_ConvertToLeftHanded;

    const aiScene* scene = importer.ReadFile(path.string(), flags);
    if (!scene || !scene->HasMeshes())
    {
        throw std::runtime_error(std::string("Assimp cannot load the model: ") +
                                 importer.GetErrorString());
    }

    XMFLOAT3 minimum{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()};
    XMFLOAT3 maximum{
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max()};

    for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex)
    {
        const aiMesh* source = scene->mMeshes[meshIndex];
        if (!source->HasPositions() || source->mNumFaces == 0)
        {
            continue;
        }

        std::vector<Vertex> vertices;
        vertices.reserve(source->mNumVertices);

        for (unsigned int i = 0; i < source->mNumVertices; ++i)
        {
            const aiVector3D& p = source->mVertices[i];
            const aiVector3D n = source->HasNormals()
                                     ? source->mNormals[i]
                                     : aiVector3D(0.0f, 1.0f, 0.0f);
            const aiVector3D uv = source->HasTextureCoords(0)
                                      ? source->mTextureCoords[0][i]
                                      : aiVector3D(0.0f, 0.0f, 0.0f);

            aiVector3D tangent(1.0f, 0.0f, 0.0f);
            aiVector3D bitangent(0.0f, 0.0f, 1.0f);
            if (source->HasTangentsAndBitangents())
            {
                tangent = source->mTangents[i];
                bitangent = source->mBitangents[i];
            }
            else if (std::abs(n.y) > 0.999f)
            {
                tangent = aiVector3D(1.0f, 0.0f, 0.0f);
                bitangent = aiVector3D(0.0f, 0.0f, n.y > 0.0f ? -1.0f : 1.0f);
            }
            else
            {
                tangent = aiVector3D(n.z, 0.0f, -n.x);
                tangent.Normalize();
                bitangent = n ^ tangent;
            }

            const aiVector3D crossNT = n ^ tangent;
            const float handedness = (crossNT * bitangent) < 0.0f ? -1.0f : 1.0f;

            vertices.push_back(Vertex{
                XMFLOAT3(p.x, p.y, p.z),
                XMFLOAT3(n.x, n.y, n.z),
                XMFLOAT2(uv.x, uv.y),
                XMFLOAT4(tangent.x, tangent.y, tangent.z, handedness)});

            minimum.x = std::min(minimum.x, p.x);
            minimum.y = std::min(minimum.y, p.y);
            minimum.z = std::min(minimum.z, p.z);
            maximum.x = std::max(maximum.x, p.x);
            maximum.y = std::max(maximum.y, p.y);
            maximum.z = std::max(maximum.z, p.z);
        }

        std::vector<std::uint32_t> indices;
        indices.reserve(static_cast<std::size_t>(source->mNumFaces) * 3u);
        for (unsigned int faceIndex = 0; faceIndex < source->mNumFaces; ++faceIndex)
        {
            const aiFace& face = source->mFaces[faceIndex];
            if (face.mNumIndices == 3)
            {
                indices.push_back(face.mIndices[0]);
                indices.push_back(face.mIndices[1]);
                indices.push_back(face.mIndices[2]);
            }
        }

        if (vertices.empty() || indices.empty())
        {
            continue;
        }

        D3D11_BUFFER_DESC vertexDesc{};
        vertexDesc.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(Vertex));
        vertexDesc.Usage = D3D11_USAGE_IMMUTABLE;
        vertexDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

        D3D11_SUBRESOURCE_DATA vertexData{};
        vertexData.pSysMem = vertices.data();

        D3D11_BUFFER_DESC indexDesc{};
        indexDesc.ByteWidth = static_cast<UINT>(indices.size() * sizeof(std::uint32_t));
        indexDesc.Usage = D3D11_USAGE_IMMUTABLE;
        indexDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;

        D3D11_SUBRESOURCE_DATA indexData{};
        indexData.pSysMem = indices.data();

        Mesh mesh;
        ThrowIfFailed(device->CreateBuffer(&vertexDesc, &vertexData, &mesh.vertexBuffer),
                      "Cannot create model vertex buffer.");
        ThrowIfFailed(device->CreateBuffer(&indexDesc, &indexData, &mesh.indexBuffer),
                      "Cannot create model index buffer.");
        mesh.indexCount = static_cast<UINT>(indices.size());
        meshes_.push_back(std::move(mesh));
    }

    if (meshes_.empty())
    {
        throw std::runtime_error("The model contains no drawable triangle meshes.");
    }

    center_ = XMFLOAT3(
        (minimum.x + maximum.x) * 0.5f,
        (minimum.y + maximum.y) * 0.5f,
        (minimum.z + maximum.z) * 0.5f);

    const float extentX = maximum.x - minimum.x;
    const float extentY = maximum.y - minimum.y;
    const float extentZ = maximum.z - minimum.z;
    radius_ = std::max({extentX, extentY, extentZ}) * 0.5f;
    radius_ = std::max(radius_, 0.0001f);
}

void Model::Draw(ID3D11DeviceContext* context) const
{
    const UINT stride = sizeof(Vertex);
    const UINT offset = 0;
    for (const Mesh& mesh : meshes_)
    {
        ID3D11Buffer* vertexBuffer = mesh.vertexBuffer.Get();
        context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
        context->IASetIndexBuffer(mesh.indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
        context->DrawIndexed(mesh.indexCount, 0, 0);
    }
}

XMMATRIX Model::NormalizationTransform() const
{
    const float scale = 2.0f / radius_;
    return XMMatrixTranslation(-center_.x, -center_.y, -center_.z) *
           XMMatrixScaling(scale, scale, scale);
}

