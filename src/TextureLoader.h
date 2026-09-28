#pragma once

#include <d3d11.h>
#include <DirectXMath.h>
#include <filesystem>
#include <wrl/client.h>

namespace TextureLoader
{
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> LoadTGA(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        const std::filesystem::path& path,
        bool srgb);

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> CreateSolid(
        ID3D11Device* device,
        const DirectX::XMFLOAT4& color,
        bool srgb);
}

