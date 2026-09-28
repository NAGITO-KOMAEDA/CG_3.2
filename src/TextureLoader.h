#pragma once

#include <d3d11.h>
#include <filesystem>
#include <wrl/client.h>

namespace TextureLoader
{
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> LoadWICTexture(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        const std::filesystem::path& path,
        bool srgb);

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> LoadDDS(
        ID3D11Device* device,
        const std::filesystem::path& path);
}

