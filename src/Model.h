#pragma once

#include <DirectXMath.h>
#include <d3d11.h>
#include <filesystem>
#include <vector>
#include <wrl/client.h>

struct Vertex
{
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT2 texCoord;
    DirectX::XMFLOAT4 tangent;
};

class Model
{
public:
    void Load(ID3D11Device* device, const std::filesystem::path& path);
    void Draw(ID3D11DeviceContext* context) const;
    DirectX::XMMATRIX NormalizationTransform() const;

private:
    struct Mesh
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
        UINT indexCount = 0;
    };

    std::vector<Mesh> meshes_;
    DirectX::XMFLOAT3 center_{0.0f, 0.0f, 0.0f};
    float radius_ = 1.0f;
};

