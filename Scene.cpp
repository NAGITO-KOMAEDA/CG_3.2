#include "Scene.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace
{
std::string Trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

struct ObjIndex
{
    int position = 0;
    int texcoord = 0;
    int normal = 0;
};

ObjIndex ParseObjIndex(const std::string& token)
{
    ObjIndex result{};
    const auto firstSlash = token.find('/');
    const auto secondSlash = firstSlash == std::string::npos ? std::string::npos : token.find('/', firstSlash + 1);

    result.position = std::stoi(token.substr(0, firstSlash));
    if (firstSlash != std::string::npos && secondSlash != firstSlash + 1)
    {
        result.texcoord = std::stoi(token.substr(firstSlash + 1, secondSlash - firstSlash - 1));
    }
    if (secondSlash != std::string::npos && secondSlash + 1 < token.size())
    {
        result.normal = std::stoi(token.substr(secondSlash + 1));
    }
    return result;
}

template <typename T>
const T& ResolveObjIndex(const std::vector<T>& values, int index)
{
    if (index == 0)
    {
        throw std::runtime_error("OBJ attribute index is missing");
    }
    const std::ptrdiff_t resolved = index > 0
        ? static_cast<std::ptrdiff_t>(index - 1)
        : static_cast<std::ptrdiff_t>(values.size()) + index;
    if (resolved < 0 || resolved >= static_cast<std::ptrdiff_t>(values.size()))
    {
        throw std::runtime_error("OBJ attribute index is out of range");
    }
    return values[static_cast<std::size_t>(resolved)];
}
}

void Scene::Load(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* commandList,
    const std::filesystem::path& objPath)
{
    if (!std::filesystem::exists(objPath))
    {
        throw std::runtime_error("Sponza OBJ was not found: " + objPath.string());
    }

    LoadMaterials(objPath.parent_path() / "sponza.mtl");
    LoadGeometry(objPath);
    CreateVertexBuffer(device, commandList);
    CreateTextures(device, commandList, objPath.parent_path());
}

void Scene::LoadMaterials(const std::filesystem::path& mtlPath)
{
    std::ifstream input(mtlPath);
    if (!input)
    {
        throw std::runtime_error("Could not open material file: " + mtlPath.string());
    }

    m_materials.clear();
    m_materialLookup.clear();
    Material* current = nullptr;
    std::string line;

    while (std::getline(input, line))
    {
        std::istringstream stream(line);
        std::string command;
        stream >> command;

        if (command == "newmtl")
        {
            std::string name;
            stream >> name;
            m_materialLookup[name] = static_cast<UINT>(m_materials.size());
            m_materials.push_back(Material{});
            current = &m_materials.back();
            current->name = name;
        }
        else if (current && command == "Kd")
        {
            stream >> current->baseColor.x >> current->baseColor.y >> current->baseColor.z;
        }
        else if (current && command == "Ks")
        {
            DirectX::XMFLOAT3 specular{};
            stream >> specular.x >> specular.y >> specular.z;
            current->specular = std::max({specular.x, specular.y, specular.z, 0.15f});
        }
        else if (current && command == "Ns")
        {
            stream >> current->shininess;
            current->shininess = std::clamp(current->shininess, 4.0f, 256.0f);
        }
        else if (current && command == "map_Kd")
        {
            std::string path;
            std::getline(stream, path);
            std::replace(path.begin(), path.end(), '\\', '/');
            current->diffuseTexture = Trim(path);
        }
    }

    if (m_materials.empty())
    {
        m_materials.push_back(Material{});
        m_materials.back().name = "default";
        m_materialLookup["default"] = 0;
    }
}

void Scene::LoadGeometry(const std::filesystem::path& objPath)
{
    std::ifstream input(objPath);
    if (!input)
    {
        throw std::runtime_error("Could not open OBJ file: " + objPath.string());
    }

    std::vector<DirectX::XMFLOAT3> positions;
    std::vector<DirectX::XMFLOAT3> normals;
    std::vector<DirectX::XMFLOAT2> texcoords;
    positions.reserve(150000);
    normals.reserve(80000);
    texcoords.reserve(90000);
    m_vertices.clear();
    m_vertices.reserve(800000);
    m_submeshes.clear();

    UINT currentMaterial = 0;
    Submesh currentSubmesh{};
    currentSubmesh.materialIndex = currentMaterial;

    auto finishSubmesh = [&]()
    {
        currentSubmesh.vertexCount = static_cast<UINT>(m_vertices.size()) - currentSubmesh.firstVertex;
        if (currentSubmesh.vertexCount > 0)
        {
            m_submeshes.push_back(currentSubmesh);
        }
        currentSubmesh.firstVertex = static_cast<UINT>(m_vertices.size());
        currentSubmesh.vertexCount = 0;
        currentSubmesh.materialIndex = currentMaterial;
    };

    std::string line;
    while (std::getline(input, line))
    {
        if (line.size() < 2 || line[0] == '#')
        {
            continue;
        }

        std::istringstream stream(line);
        std::string command;
        stream >> command;

        if (command == "v")
        {
            DirectX::XMFLOAT3 value{};
            stream >> value.x >> value.y >> value.z;
            value.z = -value.z;
            positions.push_back(value);
        }
        else if (command == "vn")
        {
            DirectX::XMFLOAT3 value{};
            stream >> value.x >> value.y >> value.z;
            value.z = -value.z;
            normals.push_back(value);
        }
        else if (command == "vt")
        {
            DirectX::XMFLOAT2 value{};
            stream >> value.x >> value.y;
            value.y = 1.0f - value.y;
            texcoords.push_back(value);
        }
        else if (command == "usemtl")
        {
            finishSubmesh();
            std::string name;
            stream >> name;
            const auto found = m_materialLookup.find(name);
            currentMaterial = found == m_materialLookup.end() ? 0u : found->second;
            currentSubmesh.materialIndex = currentMaterial;
        }
        else if (command == "f")
        {
            std::vector<ObjIndex> face;
            std::string token;
            while (stream >> token)
            {
                face.push_back(ParseObjIndex(token));
            }
            if (face.size() < 3)
            {
                continue;
            }

            const auto emitVertex = [&](const ObjIndex& index)
            {
                SceneVertex vertex{};
                vertex.position = ResolveObjIndex(positions, index.position);
                vertex.normal = index.normal == 0
                    ? DirectX::XMFLOAT3(0.0f, 1.0f, 0.0f)
                    : ResolveObjIndex(normals, index.normal);
                vertex.uv = index.texcoord == 0
                    ? DirectX::XMFLOAT2(0.0f, 0.0f)
                    : ResolveObjIndex(texcoords, index.texcoord);
                m_vertices.push_back(vertex);
            };

            for (std::size_t i = 1; i + 1 < face.size(); ++i)
            {
                emitVertex(face[0]);
                emitVertex(face[i + 1]);
                emitVertex(face[i]);
            }
        }
    }
    finishSubmesh();

    if (m_vertices.empty() || m_submeshes.empty())
    {
        throw std::runtime_error("OBJ did not contain drawable geometry");
    }
}

void Scene::CreateVertexBuffer(ID3D12Device* device, ID3D12GraphicsCommandList* commandList)
{
    const UINT64 bufferSize = static_cast<UINT64>(m_vertices.size()) * sizeof(SceneVertex);
    const auto defaultHeap = dx::HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const auto uploadHeap = dx::HeapProperties(D3D12_HEAP_TYPE_UPLOAD);

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bufferSize;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    dx::Check(
        device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&m_vertexBuffer)),
        "Create scene vertex buffer");

    dx::ComPtr<ID3D12Resource> upload;
    dx::Check(
        device->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&upload)),
        "Create vertex upload buffer");

    void* mapped = nullptr;
    const D3D12_RANGE readRange{0, 0};
    dx::Check(upload->Map(0, &readRange, &mapped), "Map vertex upload buffer");
    std::memcpy(mapped, m_vertices.data(), static_cast<std::size_t>(bufferSize));
    upload->Unmap(0, nullptr);

    commandList->CopyBufferRegion(m_vertexBuffer.Get(), 0, upload.Get(), 0, bufferSize);
    const auto barrier = dx::TransitionBarrier(
        m_vertexBuffer.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    commandList->ResourceBarrier(1, &barrier);

    m_uploadResources.push_back(upload);
    m_vertexView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
    m_vertexView.SizeInBytes = static_cast<UINT>(bufferSize);
    m_vertexView.StrideInBytes = sizeof(SceneVertex);

    m_vertices.clear();
    m_vertices.shrink_to_fit();
}

void Scene::CreateTextures(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* commandList,
    const std::filesystem::path& assetDirectory)
{
    std::unordered_set<std::wstring> uniquePaths;
    for (const auto& material : m_materials)
    {
        if (!material.diffuseTexture.empty())
        {
            uniquePaths.insert(material.diffuseTexture.lexically_normal().wstring());
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = static_cast<UINT>(uniquePaths.size()) + 1;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    dx::Check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_textureHeap)), "Create material texture heap");
    m_descriptorIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    TgaImage white{};
    white.width = 1;
    white.height = 1;
    white.rgba = {255, 255, 255, 255};
    CreateTexture(device, commandList, 0, white);

    std::unordered_map<std::wstring, UINT> descriptorByPath;
    UINT nextDescriptor = 1;
    for (auto& material : m_materials)
    {
        if (material.diffuseTexture.empty())
        {
            material.textureDescriptor = 0;
            continue;
        }

        const std::wstring key = material.diffuseTexture.lexically_normal().wstring();
        const auto found = descriptorByPath.find(key);
        if (found != descriptorByPath.end())
        {
            material.textureDescriptor = found->second;
            continue;
        }

        const auto fullPath = assetDirectory / material.diffuseTexture;
        if (!std::filesystem::exists(fullPath))
        {
            material.textureDescriptor = 0;
            continue;
        }

        const TgaImage image = LoadTga(fullPath);
        CreateTexture(device, commandList, nextDescriptor, image);
        material.textureDescriptor = nextDescriptor;
        descriptorByPath[key] = nextDescriptor;
        ++nextDescriptor;
    }
}

UINT Scene::CreateTexture(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* commandList,
    UINT descriptorIndex,
    const TgaImage& image)
{
    const auto defaultHeap = dx::HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const auto uploadHeap = dx::HeapProperties(D3D12_HEAP_TYPE_UPLOAD);

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = image.width;
    textureDesc.Height = image.height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    dx::ComPtr<ID3D12Resource> texture;
    dx::Check(
        device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &textureDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&texture)),
        "Create material texture");

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 rowSize = 0;
    UINT64 uploadSize = 0;
    device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, &rows, &rowSize, &uploadSize);

    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = uploadSize;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    dx::ComPtr<ID3D12Resource> upload;
    dx::Check(
        device->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&upload)),
        "Create texture upload buffer");

    std::uint8_t* mapped = nullptr;
    const D3D12_RANGE readRange{0, 0};
    dx::Check(upload->Map(0, &readRange, reinterpret_cast<void**>(&mapped)), "Map texture upload buffer");
    const std::size_t sourcePitch = static_cast<std::size_t>(image.width) * 4;
    for (UINT row = 0; row < image.height; ++row)
    {
        std::memcpy(
            mapped + footprint.Offset + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
            image.rgba.data() + static_cast<std::size_t>(row) * sourcePitch,
            sourcePitch);
    }
    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = texture.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    const auto barrier = dx::TransitionBarrier(
        texture.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &barrier);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;

    auto cpuHandle = m_textureHeap->GetCPUDescriptorHandleForHeapStart();
    cpuHandle.ptr += static_cast<SIZE_T>(descriptorIndex) * m_descriptorIncrement;
    device->CreateShaderResourceView(texture.Get(), &srvDesc, cpuHandle);

    m_textures.push_back(texture);
    m_uploadResources.push_back(upload);
    return descriptorIndex;
}

Scene::TgaImage Scene::LoadTga(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error("Could not open TGA texture: " + path.string());
    }

#pragma pack(push, 1)
    struct Header
    {
        std::uint8_t idLength;
        std::uint8_t colorMapType;
        std::uint8_t imageType;
        std::uint16_t colorMapStart;
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

    Header header{};
    input.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!input || header.colorMapType != 0 || (header.imageType != 2 && header.imageType != 10) ||
        (header.bitsPerPixel != 24 && header.bitsPerPixel != 32))
    {
        throw std::runtime_error("Unsupported TGA format: " + path.string());
    }

    input.seekg(header.idLength, std::ios::cur);
    const UINT channels = header.bitsPerPixel / 8;
    const std::size_t pixelCount = static_cast<std::size_t>(header.width) * header.height;
    std::vector<std::array<std::uint8_t, 4>> pixels(pixelCount);

    const auto readPixel = [&]()
    {
        std::array<std::uint8_t, 4> bgra{0, 0, 0, 255};
        input.read(reinterpret_cast<char*>(bgra.data()), channels);
        return std::array<std::uint8_t, 4>{bgra[2], bgra[1], bgra[0], bgra[3]};
    };

    if (header.imageType == 2)
    {
        for (auto& pixel : pixels)
        {
            pixel = readPixel();
        }
    }
    else
    {
        std::size_t outputIndex = 0;
        while (outputIndex < pixelCount && input)
        {
            std::uint8_t packet = 0;
            input.read(reinterpret_cast<char*>(&packet), 1);
            const std::size_t count = (packet & 0x7Fu) + 1u;
            if (packet & 0x80u)
            {
                const auto pixel = readPixel();
                for (std::size_t i = 0; i < count && outputIndex < pixelCount; ++i)
                {
                    pixels[outputIndex++] = pixel;
                }
            }
            else
            {
                for (std::size_t i = 0; i < count && outputIndex < pixelCount; ++i)
                {
                    pixels[outputIndex++] = readPixel();
                }
            }
        }
    }

    if (!input)
    {
        throw std::runtime_error("Unexpected end of TGA file: " + path.string());
    }

    TgaImage image{};
    image.width = header.width;
    image.height = header.height;
    image.rgba.resize(pixelCount * 4);
    const bool topOrigin = (header.descriptor & 0x20u) != 0;

    for (UINT y = 0; y < image.height; ++y)
    {
        const UINT sourceY = topOrigin ? y : (image.height - 1u - y);
        for (UINT x = 0; x < image.width; ++x)
        {
            const auto& pixel = pixels[static_cast<std::size_t>(sourceY) * image.width + x];
            const std::size_t destination = (static_cast<std::size_t>(y) * image.width + x) * 4;
            std::copy(pixel.begin(), pixel.end(), image.rgba.begin() + static_cast<std::ptrdiff_t>(destination));
        }
    }
    return image;
}

void Scene::Draw(
    ID3D12GraphicsCommandList* commandList,
    UINT textureRootParameter,
    UINT materialRootParameter) const
{
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 1, &m_vertexView);

    const auto gpuStart = m_textureHeap->GetGPUDescriptorHandleForHeapStart();
    for (const auto& submesh : m_submeshes)
    {
        const Material& material = m_materials[submesh.materialIndex];
        auto textureHandle = gpuStart;
        textureHandle.ptr += static_cast<UINT64>(material.textureDescriptor) * m_descriptorIncrement;

        struct MaterialConstants
        {
            DirectX::XMFLOAT4 baseColor;
            float specular;
            float shininess;
            float padding[2];
        } constants{
            material.baseColor,
            material.specular,
            material.shininess,
            {0.0f, 0.0f},
        };

        commandList->SetGraphicsRootDescriptorTable(textureRootParameter, textureHandle);
        commandList->SetGraphicsRoot32BitConstants(materialRootParameter, 8, &constants, 0);
        commandList->DrawInstanced(submesh.vertexCount, 1, submesh.firstVertex, 0);
    }
}

