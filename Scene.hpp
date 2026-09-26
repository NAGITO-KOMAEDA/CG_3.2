#pragma once

#include "DxUtil.hpp"

#include <DirectXMath.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

struct SceneVertex
{
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT2 uv;
};

class Scene
{
public:
    void Load(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* commandList,
        const std::filesystem::path& objPath);

    void Draw(
        ID3D12GraphicsCommandList* commandList,
        UINT textureRootParameter,
        UINT materialRootParameter) const;

    ID3D12DescriptorHeap* TextureHeap() const { return m_textureHeap.Get(); }
    void ReleaseUploadResources() { m_uploadResources.clear(); }

private:
    struct Material
    {
        std::string name;
        DirectX::XMFLOAT4 baseColor = {1.0f, 1.0f, 1.0f, 1.0f};
        float specular = 0.25f;
        float shininess = 32.0f;
        std::filesystem::path diffuseTexture;
        UINT textureDescriptor = 0;
    };

    struct Submesh
    {
        UINT firstVertex = 0;
        UINT vertexCount = 0;
        UINT materialIndex = 0;
    };

    struct TgaImage
    {
        UINT width = 0;
        UINT height = 0;
        std::vector<std::uint8_t> rgba;
    };

    void LoadMaterials(const std::filesystem::path& mtlPath);
    void LoadGeometry(const std::filesystem::path& objPath);
    void CreateVertexBuffer(ID3D12Device* device, ID3D12GraphicsCommandList* commandList);
    void CreateTextures(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* commandList,
        const std::filesystem::path& assetDirectory);
    UINT CreateTexture(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* commandList,
        UINT descriptorIndex,
        const TgaImage& image);
    static TgaImage LoadTga(const std::filesystem::path& path);

    std::vector<SceneVertex> m_vertices;
    std::vector<Submesh> m_submeshes;
    std::vector<Material> m_materials;
    std::unordered_map<std::string, UINT> m_materialLookup;

    dx::ComPtr<ID3D12Resource> m_vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vertexView{};
    dx::ComPtr<ID3D12DescriptorHeap> m_textureHeap;
    std::vector<dx::ComPtr<ID3D12Resource>> m_textures;
    std::vector<dx::ComPtr<ID3D12Resource>> m_uploadResources;
    UINT m_descriptorIncrement = 0;
};

