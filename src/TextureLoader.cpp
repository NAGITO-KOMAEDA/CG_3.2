#include "TextureLoader.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
    void ThrowIfFailed(HRESULT hr, const char* message)
    {
        if (FAILED(hr))
            throw std::runtime_error(message);
    }

#pragma pack(push, 1)
    struct TgaHeader
    {
        std::uint8_t idLength;
        std::uint8_t colorMapType;
        std::uint8_t imageType;
        std::uint16_t colorMapOrigin;
        std::uint16_t colorMapLength;
        std::uint8_t colorMapDepth;
        std::uint16_t xOrigin;
        std::uint16_t yOrigin;
        std::uint16_t width;
        std::uint16_t height;
        std::uint8_t bitsPerPixel;
        std::uint8_t descriptor;
    };
#pragma pack(pop)

    ComPtr<ID3D11ShaderResourceView> CreateTexture(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        UINT width,
        UINT height,
        const void* pixels,
        bool srgb,
        bool generateMips)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = generateMips ? 0u : 1u;
        desc.ArraySize = 1;
        desc.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                           : DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE |
                         (generateMips ? D3D11_BIND_RENDER_TARGET : 0u);
        desc.MiscFlags = generateMips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0u;

        ComPtr<ID3D11Texture2D> texture;
        if (generateMips)
        {
            ThrowIfFailed(device->CreateTexture2D(&desc, nullptr, &texture),
                          "Cannot create TGA texture.");
            context->UpdateSubresource(texture.Get(), 0, nullptr, pixels, width * 4u, 0);
        }
        else
        {
            D3D11_SUBRESOURCE_DATA data{};
            data.pSysMem = pixels;
            data.SysMemPitch = width * 4u;
            ThrowIfFailed(device->CreateTexture2D(&desc, &data, &texture),
                          "Cannot create solid texture.");
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = generateMips ? UINT(-1) : 1u;

        ComPtr<ID3D11ShaderResourceView> result;
        ThrowIfFailed(device->CreateShaderResourceView(texture.Get(), &srvDesc, &result),
                      "Cannot create texture view.");
        if (generateMips)
            context->GenerateMips(result.Get());
        return result;
    }
}

ComPtr<ID3D11ShaderResourceView> TextureLoader::LoadTGA(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    const std::filesystem::path& path,
    bool srgb)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("Cannot open TGA texture: " + path.string());

    TgaHeader header{};
    stream.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!stream || header.colorMapType != 0 || header.imageType != 2 ||
        (header.bitsPerPixel != 24 && header.bitsPerPixel != 32) ||
        header.width == 0 || header.height == 0)
    {
        throw std::runtime_error("Unsupported TGA texture: " + path.string());
    }

    stream.seekg(header.idLength, std::ios::cur);
    const UINT sourceBytesPerPixel = header.bitsPerPixel / 8u;
    const std::size_t sourceSize = static_cast<std::size_t>(header.width) *
                                   header.height * sourceBytesPerPixel;
    std::vector<std::uint8_t> source(sourceSize);
    if (!stream.read(reinterpret_cast<char*>(source.data()),
                     static_cast<std::streamsize>(source.size())))
    {
        throw std::runtime_error("TGA texture data is truncated: " + path.string());
    }

    const bool topOrigin = (header.descriptor & 0x20u) != 0;
    const bool rightOrigin = (header.descriptor & 0x10u) != 0;
    std::vector<std::uint8_t> rgba(
        static_cast<std::size_t>(header.width) * header.height * 4u);

    for (UINT sourceY = 0; sourceY < header.height; ++sourceY)
    {
        for (UINT sourceX = 0; sourceX < header.width; ++sourceX)
        {
            const UINT targetX = rightOrigin ? header.width - 1u - sourceX : sourceX;
            const UINT targetY = topOrigin ? sourceY : header.height - 1u - sourceY;
            const std::size_t sourceOffset =
                (static_cast<std::size_t>(sourceY) * header.width + sourceX) * sourceBytesPerPixel;
            const std::size_t targetOffset =
                (static_cast<std::size_t>(targetY) * header.width + targetX) * 4u;
            rgba[targetOffset + 0] = source[sourceOffset + 2];
            rgba[targetOffset + 1] = source[sourceOffset + 1];
            rgba[targetOffset + 2] = source[sourceOffset + 0];
            rgba[targetOffset + 3] = sourceBytesPerPixel == 4 ? source[sourceOffset + 3] : 255u;
        }
    }

    return CreateTexture(device, context, header.width, header.height, rgba.data(), srgb, true);
}

ComPtr<ID3D11ShaderResourceView> TextureLoader::CreateSolid(
    ID3D11Device* device,
    const DirectX::XMFLOAT4& color,
    bool srgb)
{
    auto toByte = [](float value) {
        return static_cast<std::uint8_t>(
            std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    const std::uint8_t pixel[4] = {
        toByte(color.x), toByte(color.y), toByte(color.z), toByte(color.w)};
    return CreateTexture(device, nullptr, 1, 1, pixel, srgb, false);
}

