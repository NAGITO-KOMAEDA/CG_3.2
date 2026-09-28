#pragma once

#include <DirectXMath.h>
#include <d3d11.h>
#include <filesystem>
#include <vector>
#include <wrl/client.h>

struct SceneVertex
{
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT2 texCoord;
    DirectX::XMFLOAT4 tangent;
};

class SceneModel
{
public:
    void Load(ID3D11Device* device,
              ID3D11DeviceContext* context,
              const std::filesystem::path& path);
    void Draw(ID3D11DeviceContext* context) const;
    DirectX::XMMATRIX NormalizationTransform() const;

private:
    struct Material
    {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> albedo;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> normal;
    };

    struct Mesh
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
        UINT indexCount = 0;
        UINT materialIndex = 0;
    };

    std::vector<Material> materials_;
    std::vector<Mesh> meshes_;
    DirectX::XMFLOAT3 center_{0.0f, 0.0f, 0.0f};
    float radius_ = 1.0f;
};

