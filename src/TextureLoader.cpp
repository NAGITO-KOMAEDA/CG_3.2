#include "TextureLoader.h"

#include <wincodec.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

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

#pragma pack(push, 1)
    struct DDSPixelFormat
    {
        std::uint32_t size;
        std::uint32_t flags;
        std::uint32_t fourCC;
        std::uint32_t rgbBitCount;
        std::uint32_t rMask;
        std::uint32_t gMask;
        std::uint32_t bMask;
        std::uint32_t aMask;
    };

    struct DDSHeader
    {
        std::uint32_t size;
        std::uint32_t flags;
        std::uint32_t height;
        std::uint32_t width;
        std::uint32_t pitchOrLinearSize;
        std::uint32_t depth;
        std::uint32_t mipMapCount;
        std::uint32_t reserved1[11];
        DDSPixelFormat pixelFormat;
        std::uint32_t caps;
        std::uint32_t caps2;
        std::uint32_t caps3;
        std::uint32_t caps4;
        std::uint32_t reserved2;
    };

    struct DDSHeaderDX10
    {
        DXGI_FORMAT format;
        D3D11_RESOURCE_DIMENSION resourceDimension;
        std::uint32_t miscFlag;
        std::uint32_t arraySize;
        std::uint32_t miscFlags2;
    };
#pragma pack(pop)

    constexpr std::uint32_t MakeFourCC(char a, char b, char c, char d)
    {
        return static_cast<std::uint32_t>(a) |
               (static_cast<std::uint32_t>(b) << 8u) |
               (static_cast<std::uint32_t>(c) << 16u) |
               (static_cast<std::uint32_t>(d) << 24u);
    }

    struct SurfaceInfo
    {
        std::size_t rowBytes = 0;
        std::size_t numRows = 0;
        std::size_t numBytes = 0;
    };

    SurfaceInfo GetSurfaceInfo(std::uint32_t width, std::uint32_t height, DXGI_FORMAT format)
    {
        SurfaceInfo result;
        if (format == DXGI_FORMAT_BC6H_UF16)
        {
            const std::size_t blocksWide = std::max<std::size_t>(1u, (width + 3u) / 4u);
            const std::size_t blocksHigh = std::max<std::size_t>(1u, (height + 3u) / 4u);
            result.rowBytes = blocksWide * 16u;
            result.numRows = blocksHigh;
            result.numBytes = result.rowBytes * result.numRows;
            return result;
        }

        if (format == DXGI_FORMAT_R32G32_FLOAT)
        {
            result.rowBytes = static_cast<std::size_t>(width) * 8u;
            result.numRows = height;
            result.numBytes = result.rowBytes * result.numRows;
            return result;
        }

        throw std::runtime_error("Unsupported DDS format. Expected BC6H_UF16 or R32G32_FLOAT.");
    }
}

ComPtr<ID3D11ShaderResourceView> TextureLoader::LoadWICTexture(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    const std::filesystem::path& path,
    bool srgb)
{
    ComPtr<IWICImagingFactory> factory;
    ThrowIfFailed(
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&factory)),
        "Cannot create WIC imaging factory.");

    ComPtr<IWICBitmapDecoder> decoder;
    ThrowIfFailed(
        factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                           WICDecodeMetadataCacheOnDemand, &decoder),
        "Cannot open material texture.");

    ComPtr<IWICBitmapFrameDecode> frame;
    ThrowIfFailed(decoder->GetFrame(0, &frame), "Cannot read material texture frame.");

    UINT width = 0;
    UINT height = 0;
    ThrowIfFailed(frame->GetSize(&width, &height), "Cannot read material texture dimensions.");

    ComPtr<IWICFormatConverter> converter;
    ThrowIfFailed(factory->CreateFormatConverter(&converter), "Cannot create WIC format converter.");
    ThrowIfFailed(
        converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                              WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeCustom),
        "Cannot convert material texture to RGBA.");

    const UINT rowPitch = width * 4u;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(rowPitch) * height);
    ThrowIfFailed(
        converter->CopyPixels(nullptr, rowPitch, static_cast<UINT>(pixels.size()), pixels.data()),
        "Cannot copy material texture pixels.");

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.MipLevels = 0;
    textureDesc.ArraySize = 1;
    textureDesc.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                              : DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    textureDesc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    ComPtr<ID3D11Texture2D> texture;
    ThrowIfFailed(device->CreateTexture2D(&textureDesc, nullptr, &texture),
                  "Cannot create material texture.");
    context->UpdateSubresource(texture.Get(), 0, nullptr, pixels.data(), rowPitch, 0);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = textureDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = UINT(-1);

    ComPtr<ID3D11ShaderResourceView> result;
    ThrowIfFailed(device->CreateShaderResourceView(texture.Get(), &srvDesc, &result),
                  "Cannot create material texture view.");
    context->GenerateMips(result.Get());
    return result;
}

ComPtr<ID3D11ShaderResourceView> TextureLoader::LoadDDS(
    ID3D11Device* device,
    const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
    {
        throw std::runtime_error("Cannot open DDS texture.");
    }

    const std::streamsize fileSize = stream.tellg();
    if (fileSize < static_cast<std::streamsize>(4 + sizeof(DDSHeader) + sizeof(DDSHeaderDX10)))
    {
        throw std::runtime_error("DDS texture is too small.");
    }

    stream.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileSize));
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), fileSize))
    {
        throw std::runtime_error("Cannot read DDS texture.");
    }

    constexpr std::uint32_t ddsMagic = MakeFourCC('D', 'D', 'S', ' ');
    const auto magic = *reinterpret_cast<const std::uint32_t*>(bytes.data());
    if (magic != ddsMagic)
    {
        throw std::runtime_error("Invalid DDS signature.");
    }

    const auto* header = reinterpret_cast<const DDSHeader*>(bytes.data() + 4u);
    if (header->size != sizeof(DDSHeader) ||
        header->pixelFormat.size != sizeof(DDSPixelFormat) ||
        header->pixelFormat.fourCC != MakeFourCC('D', 'X', '1', '0'))
    {
        throw std::runtime_error("DDS must contain a DX10 header.");
    }

    const auto* header10 = reinterpret_cast<const DDSHeaderDX10*>(
        bytes.data() + 4u + sizeof(DDSHeader));
    if (header10->resourceDimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D ||
        header10->arraySize == 0)
    {
        throw std::runtime_error("Only 2D and cubemap DDS textures are supported.");
    }

    const bool isCube = (header10->miscFlag & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
    const UINT mipCount = std::max(1u, header->mipMapCount);
    const UINT arraySize = header10->arraySize * (isCube ? 6u : 1u);
    const std::size_t dataOffset = 4u + sizeof(DDSHeader) + sizeof(DDSHeaderDX10);

    std::vector<D3D11_SUBRESOURCE_DATA> subresources(
        static_cast<std::size_t>(mipCount) * arraySize);
    std::size_t cursor = dataOffset;

    for (UINT slice = 0; slice < arraySize; ++slice)
    {
        std::uint32_t width = header->width;
        std::uint32_t height = header->height;
        for (UINT mip = 0; mip < mipCount; ++mip)
        {
            const SurfaceInfo info = GetSurfaceInfo(width, height, header10->format);
            if (cursor + info.numBytes > bytes.size())
            {
                throw std::runtime_error("DDS texture data is truncated.");
            }

            const UINT subresource = D3D11CalcSubresource(mip, slice, mipCount);
            subresources[subresource].pSysMem = bytes.data() + cursor;
            subresources[subresource].SysMemPitch = static_cast<UINT>(info.rowBytes);
            subresources[subresource].SysMemSlicePitch = static_cast<UINT>(info.numBytes);

            cursor += info.numBytes;
            width = std::max(1u, width / 2u);
            height = std::max(1u, height / 2u);
        }
    }

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = header->width;
    textureDesc.Height = header->height;
    textureDesc.MipLevels = mipCount;
    textureDesc.ArraySize = arraySize;
    textureDesc.Format = header10->format;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_IMMUTABLE;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    textureDesc.MiscFlags = isCube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;

    ComPtr<ID3D11Texture2D> texture;
    ThrowIfFailed(device->CreateTexture2D(&textureDesc, subresources.data(), &texture),
                  "Cannot create DDS texture.");

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = textureDesc.Format;
    if (isCube)
    {
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        srvDesc.TextureCube.MostDetailedMip = 0;
        srvDesc.TextureCube.MipLevels = mipCount;
    }
    else
    {
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = mipCount;
    }

    ComPtr<ID3D11ShaderResourceView> result;
    ThrowIfFailed(device->CreateShaderResourceView(texture.Get(), &srvDesc, &result),
                  "Cannot create DDS shader resource view.");
    return result;
}

