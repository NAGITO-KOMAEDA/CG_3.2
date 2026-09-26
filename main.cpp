#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

struct ObjIndex
{
    int v = -1;
    int t = -1;
    int n = -1;

    bool operator==(const ObjIndex& other) const
    {
        return v == other.v && t == other.t && n == other.n;
    }
};

struct ObjIndexHash
{
    size_t operator()(const ObjIndex& value) const
    {
        size_t h = static_cast<size_t>(value.v + 1);
        h = h * 1000003u ^ static_cast<size_t>(value.t + 1);
        h = h * 1000003u ^ static_cast<size_t>(value.n + 1);
        return h;
    }
};

struct Vertex
{
    XMFLOAT3 position;
    XMFLOAT3 normal;
    XMFLOAT2 texcoord;
};

struct MaterialData
{
    std::string name;
    XMFLOAT4 color = {1, 1, 1, 1};
    std::filesystem::path texturePath;
    ComPtr<ID3D11ShaderResourceView> texture;
    uint32_t indexStart = 0;
    uint32_t indexCount = 0;
};

struct SceneData
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<MaterialData> materials;
    XMFLOAT3 minimum = {};
    XMFLOAT3 maximum = {};
};

struct Constants
{
    XMFLOAT4X4 worldViewProjection;
    XMFLOAT4X4 world;
    XMFLOAT4 tilingOffset;
    XMFLOAT4 lightPosition;
    XMFLOAT4 cameraPosition;
    XMFLOAT4 diffuseColor;
};

struct TgaImage
{
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;
};

static bool gRunning = true;
static uint32_t gClientWidth = 1280;
static uint32_t gClientHeight = 800;
static bool gResizePending = false;
static bool gRightButton = false;
static POINT gLastMouse = {};
static float gYaw = 0.8f;
static float gPitch = 0.15f;
static float gDistance = 2.8f;
static float gWheel = 0.0f;
static float gTiling = 1.0f;
static bool gAnimation = true;

static void Check(HRESULT result, const char* text)
{
    if (FAILED(result))
        throw std::runtime_error(text + std::string(" (HRESULT 0x") +
            [] (HRESULT hr)
            {
                char buffer[16];
                std::snprintf(buffer, sizeof(buffer), "%08X", static_cast<unsigned int>(hr));
                return std::string(buffer);
            }(result) + ")");
}

static void PumpMessages()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT)
            gRunning = false;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        gRunning = false;
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
        {
            gClientWidth = std::max(1u, static_cast<uint32_t>(LOWORD(lParam)));
            gClientHeight = std::max(1u, static_cast<uint32_t>(HIWORD(lParam)));
            gResizePending = true;
        }
        return 0;
    case WM_RBUTTONDOWN:
        gRightButton = true;
        gLastMouse = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        SetCapture(window);
        return 0;
    case WM_RBUTTONUP:
        gRightButton = false;
        ReleaseCapture();
        return 0;
    case WM_MOUSEMOVE:
        if (gRightButton)
        {
            POINT current = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            gYaw -= static_cast<float>(current.x - gLastMouse.x) * 0.006f;
            gPitch += static_cast<float>(current.y - gLastMouse.y) * 0.006f;
            gPitch = std::clamp(gPitch, -1.45f, 1.45f);
            gLastMouse = current;
        }
        return 0;
    case WM_MOUSEWHEEL:
        gWheel += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
        return 0;
    case WM_KEYDOWN:
        if ((lParam & (1u << 30)) == 0)
        {
            if (wParam == VK_ESCAPE)
                DestroyWindow(window);
            else if (wParam == VK_SPACE)
                gAnimation = !gAnimation;
            else if (wParam == VK_OEM_PLUS || wParam == VK_ADD)
                gTiling = std::min(32.0f, gTiling + 1.0f);
            else if (wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT)
                gTiling = std::max(1.0f, gTiling - 1.0f);
            else if (wParam == 'R')
                gTiling = 1.0f;
        }
        return 0;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

static std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Cannot open " + path.string());

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::string data(static_cast<size_t>(size), '\0');
    if (size > 0)
        file.read(data.data(), size);
    return data;
}

static void SkipSpaces(const char*& cursor, const char* end)
{
    while (cursor < end && (*cursor == ' ' || *cursor == '\t'))
        ++cursor;
}

static int ReadInteger(const char*& cursor, const char* end)
{
    int sign = 1;
    if (cursor < end && *cursor == '-')
    {
        sign = -1;
        ++cursor;
    }
    else if (cursor < end && *cursor == '+')
    {
        ++cursor;
    }

    int value = 0;
    while (cursor < end && *cursor >= '0' && *cursor <= '9')
    {
        value = value * 10 + (*cursor - '0');
        ++cursor;
    }
    return value * sign;
}

static float ReadFloat(const char*& cursor, const char* end)
{
    SkipSpaces(cursor, end);
    char* next = nullptr;
    float value = std::strtof(cursor, &next);
    cursor = next;
    return value;
}

static int ResolveIndex(int value, int count)
{
    if (value > 0)
        return value - 1;
    if (value < 0)
        return count + value;
    return -1;
}

static ObjIndex ReadObjIndex(const char*& cursor, const char* end, int positionCount, int texcoordCount, int normalCount)
{
    ObjIndex result;
    SkipSpaces(cursor, end);
    result.v = ResolveIndex(ReadInteger(cursor, end), positionCount);

    if (cursor < end && *cursor == '/')
    {
        ++cursor;
        if (cursor < end && *cursor != '/')
            result.t = ResolveIndex(ReadInteger(cursor, end), texcoordCount);
        if (cursor < end && *cursor == '/')
        {
            ++cursor;
            if (cursor < end && *cursor != ' ' && *cursor != '\t' && *cursor != '\r')
                result.n = ResolveIndex(ReadInteger(cursor, end), normalCount);
        }
    }
    return result;
}

static std::string ReadName(const char* begin, const char* end)
{
    while (begin < end && (*begin == ' ' || *begin == '\t'))
        ++begin;
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
        --end;
    return std::string(begin, end);
}

static std::string FindMtlName(const std::string& obj)
{
    const char* cursor = obj.data();
    const char* end = cursor + obj.size();
    while (cursor < end)
    {
        const char* line = cursor;
        while (cursor < end && *cursor != '\n')
            ++cursor;
        if (cursor - line >= 7 && std::memcmp(line, "mtllib ", 7) == 0)
            return ReadName(line + 7, cursor);
        if (cursor < end)
            ++cursor;
    }
    return {};
}

static std::vector<MaterialData> LoadMaterials(const std::filesystem::path& path, std::unordered_map<std::string, uint32_t>& ids)
{
    std::vector<MaterialData> materials;
    std::ifstream file(path);
    std::string line;
    MaterialData* current = nullptr;

    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind("newmtl ", 0) == 0)
        {
            MaterialData material;
            material.name = line.substr(7);
            ids[material.name] = static_cast<uint32_t>(materials.size());
            materials.push_back(std::move(material));
            current = &materials.back();
        }
        else if (current && line.rfind("Kd ", 0) == 0)
        {
            const char* cursor = line.data() + 3;
            const char* end = line.data() + line.size();
            current->color.x = ReadFloat(cursor, end);
            current->color.y = ReadFloat(cursor, end);
            current->color.z = ReadFloat(cursor, end);
        }
        else if (current && line.rfind("map_Kd ", 0) == 0)
        {
            current->texturePath = std::filesystem::path(line.substr(7));
        }
    }

    if (materials.empty())
    {
        ids["default"] = 0;
        materials.push_back({});
        materials.back().name = "default";
    }
    return materials;
}

static uint32_t AddVertex(const ObjIndex& index, SceneData& scene, std::unordered_map<ObjIndex, uint32_t, ObjIndexHash>& lookup,
    const std::vector<XMFLOAT3>& positions, const std::vector<XMFLOAT2>& texcoords, const std::vector<XMFLOAT3>& normals)
{
    auto found = lookup.find(index);
    if (found != lookup.end())
        return found->second;

    Vertex vertex{};
    vertex.position = positions.at(static_cast<size_t>(index.v));
    vertex.texcoord = index.t >= 0 ? texcoords.at(static_cast<size_t>(index.t)) : XMFLOAT2{0, 0};
    vertex.normal = index.n >= 0 ? normals.at(static_cast<size_t>(index.n)) : XMFLOAT3{0, 1, 0};
    uint32_t result = static_cast<uint32_t>(scene.vertices.size());
    scene.vertices.push_back(vertex);
    lookup.emplace(index, result);
    return result;
}

static SceneData LoadObj(const std::filesystem::path& objPath)
{
    SceneData scene;
    std::string data = ReadFile(objPath);
    std::unordered_map<std::string, uint32_t> materialIds;
    std::string mtlName = FindMtlName(data);
    if (!mtlName.empty())
        scene.materials = LoadMaterials(objPath.parent_path() / mtlName, materialIds);
    if (scene.materials.empty())
    {
        materialIds["default"] = 0;
        scene.materials.push_back({});
        scene.materials.back().name = "default";
    }

    std::vector<XMFLOAT3> positions;
    std::vector<XMFLOAT2> texcoords;
    std::vector<XMFLOAT3> normals;
    std::vector<std::vector<uint32_t>> materialIndices(scene.materials.size());
    std::unordered_map<ObjIndex, uint32_t, ObjIndexHash> lookup;
    positions.reserve(160000);
    texcoords.reserve(160000);
    normals.reserve(160000);
    scene.vertices.reserve(300000);
    lookup.reserve(400000);

    XMFLOAT3 minimum = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    XMFLOAT3 maximum = {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
    uint32_t currentMaterial = 0;
    size_t lineNumber = 0;
    const char* cursor = data.data();
    const char* end = cursor + data.size();

    while (cursor < end && gRunning)
    {
        const char* line = cursor;
        while (cursor < end && *cursor != '\n')
            ++cursor;
        const char* lineEnd = cursor;

        if (lineEnd - line >= 2 && line[0] == 'v' && line[1] == ' ')
        {
            const char* value = line + 2;
            XMFLOAT3 position = {ReadFloat(value, lineEnd), ReadFloat(value, lineEnd), ReadFloat(value, lineEnd)};
            positions.push_back(position);
            minimum.x = std::min(minimum.x, position.x);
            minimum.y = std::min(minimum.y, position.y);
            minimum.z = std::min(minimum.z, position.z);
            maximum.x = std::max(maximum.x, position.x);
            maximum.y = std::max(maximum.y, position.y);
            maximum.z = std::max(maximum.z, position.z);
        }
        else if (lineEnd - line >= 3 && line[0] == 'v' && line[1] == 't' && line[2] == ' ')
        {
            const char* value = line + 3;
            texcoords.push_back({ReadFloat(value, lineEnd), 1.0f - ReadFloat(value, lineEnd)});
        }
        else if (lineEnd - line >= 3 && line[0] == 'v' && line[1] == 'n' && line[2] == ' ')
        {
            const char* value = line + 3;
            normals.push_back({ReadFloat(value, lineEnd), ReadFloat(value, lineEnd), ReadFloat(value, lineEnd)});
        }
        else if (lineEnd - line >= 7 && std::memcmp(line, "usemtl ", 7) == 0)
        {
            auto found = materialIds.find(ReadName(line + 7, lineEnd));
            if (found != materialIds.end())
                currentMaterial = found->second;
        }
        else if (lineEnd - line >= 2 && line[0] == 'f' && line[1] == ' ')
        {
            ObjIndex polygon[64];
            int count = 0;
            const char* value = line + 2;
            while (value < lineEnd && count < 64)
            {
                SkipSpaces(value, lineEnd);
                if (value >= lineEnd || *value == '\r')
                    break;
                polygon[count++] = ReadObjIndex(value, lineEnd, static_cast<int>(positions.size()), static_cast<int>(texcoords.size()), static_cast<int>(normals.size()));
            }
            for (int i = 2; i < count; ++i)
            {
                auto& output = materialIndices[currentMaterial];
                output.push_back(AddVertex(polygon[0], scene, lookup, positions, texcoords, normals));
                output.push_back(AddVertex(polygon[i - 1], scene, lookup, positions, texcoords, normals));
                output.push_back(AddVertex(polygon[i], scene, lookup, positions, texcoords, normals));
            }
        }

        if (cursor < end)
            ++cursor;
        if ((++lineNumber & 16383u) == 0)
            PumpMessages();
    }

    scene.minimum = minimum;
    scene.maximum = maximum;
    size_t totalIndices = 0;
    for (const auto& indices : materialIndices)
        totalIndices += indices.size();
    scene.indices.reserve(totalIndices);

    for (size_t i = 0; i < materialIndices.size(); ++i)
    {
        scene.materials[i].indexStart = static_cast<uint32_t>(scene.indices.size());
        scene.materials[i].indexCount = static_cast<uint32_t>(materialIndices[i].size());
        scene.indices.insert(scene.indices.end(), materialIndices[i].begin(), materialIndices[i].end());
    }

    if (scene.vertices.empty() || scene.indices.empty())
        throw std::runtime_error("OBJ file contains no triangles");
    return scene;
}

static TgaImage LoadTga(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot open texture " + path.string());

    uint8_t header[18]{};
    file.read(reinterpret_cast<char*>(header), sizeof(header));
    uint8_t idLength = header[0];
    uint8_t colorMapType = header[1];
    uint8_t imageType = header[2];
    uint16_t width = static_cast<uint16_t>(header[12] | (header[13] << 8));
    uint16_t height = static_cast<uint16_t>(header[14] | (header[15] << 8));
    uint8_t bits = header[16];
    bool topOrigin = (header[17] & 0x20) != 0;
    if (colorMapType != 0 || (imageType != 2 && imageType != 10) || (bits != 24 && bits != 32) || width == 0 || height == 0)
        throw std::runtime_error("Unsupported TGA " + path.string());

    file.seekg(idLength, std::ios::cur);
    uint32_t bytesPerPixel = bits / 8;
    uint32_t pixelCount = static_cast<uint32_t>(width) * height;
    std::vector<uint8_t> source(static_cast<size_t>(pixelCount) * bytesPerPixel);

    if (imageType == 2)
    {
        file.read(reinterpret_cast<char*>(source.data()), static_cast<std::streamsize>(source.size()));
    }
    else
    {
        uint32_t outputPixel = 0;
        while (outputPixel < pixelCount)
        {
            uint8_t packet = 0;
            file.read(reinterpret_cast<char*>(&packet), 1);
            uint32_t count = (packet & 0x7f) + 1;
            if (packet & 0x80)
            {
                uint8_t pixel[4]{};
                file.read(reinterpret_cast<char*>(pixel), bytesPerPixel);
                for (uint32_t i = 0; i < count && outputPixel < pixelCount; ++i, ++outputPixel)
                    std::memcpy(source.data() + static_cast<size_t>(outputPixel) * bytesPerPixel, pixel, bytesPerPixel);
            }
            else
            {
                size_t bytes = static_cast<size_t>(count) * bytesPerPixel;
                file.read(reinterpret_cast<char*>(source.data() + static_cast<size_t>(outputPixel) * bytesPerPixel), bytes);
                outputPixel += count;
            }
        }
    }

    TgaImage image;
    image.width = width;
    image.height = height;
    image.pixels.resize(static_cast<size_t>(pixelCount) * 4);
    for (uint32_t y = 0; y < height; ++y)
    {
        uint32_t sourceY = topOrigin ? y : height - 1 - y;
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint8_t* input = source.data() + (static_cast<size_t>(sourceY) * width + x) * bytesPerPixel;
            uint8_t* output = image.pixels.data() + (static_cast<size_t>(y) * width + x) * 4;
            output[0] = input[2];
            output[1] = input[1];
            output[2] = input[0];
            output[3] = bytesPerPixel == 4 ? input[3] : 255;
        }
    }
    return image;
}

static ComPtr<ID3D11ShaderResourceView> CreateTexture(ID3D11Device* device, ID3D11DeviceContext* context, const TgaImage& image)
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = image.width;
    description.Height = image.height;
    description.MipLevels = 0;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    description.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    ComPtr<ID3D11Texture2D> texture;
    Check(device->CreateTexture2D(&description, nullptr, &texture), "CreateTexture2D failed");
    context->UpdateSubresource(texture.Get(), 0, nullptr, image.pixels.data(), image.width * 4, 0);

    D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
    viewDescription.Format = description.Format;
    viewDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    viewDescription.Texture2D.MostDetailedMip = 0;
    viewDescription.Texture2D.MipLevels = UINT(-1);

    ComPtr<ID3D11ShaderResourceView> view;
    Check(device->CreateShaderResourceView(texture.Get(), &viewDescription, &view), "CreateShaderResourceView failed");
    context->GenerateMips(view.Get());
    return view;
}

static ComPtr<ID3DBlob> CompileShader(const char* source, const char* entry, const char* profile)
{
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    HRESULT result = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(result))
    {
        if (errors)
            std::cerr.write(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        Check(result, "D3DCompile failed");
    }
    return code;
}

class Renderer
{
public:
    void Initialize(HWND window, uint32_t width, uint32_t height)
    {
        DXGI_SWAP_CHAIN_DESC swapDescription{};
        swapDescription.BufferCount = 2;
        swapDescription.BufferDesc.Width = width;
        swapDescription.BufferDesc.Height = height;
        swapDescription.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDescription.OutputWindow = window;
        swapDescription.SampleDesc.Count = 1;
        swapDescription.Windowed = TRUE;
        swapDescription.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL obtained{};
        HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, requested, 2,
            D3D11_SDK_VERSION, &swapDescription, &swapChain_, &device_, &obtained, &context_);
        if (result == E_INVALIDARG)
            result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, requested + 1, 1,
                D3D11_SDK_VERSION, &swapDescription, &swapChain_, &device_, &obtained, &context_);
        Check(result, "D3D11CreateDeviceAndSwapChain failed");
        Resize(width, height);
        CreatePipeline();
    }

    void Upload(SceneData& scene, const std::filesystem::path& basePath)
    {
        D3D11_BUFFER_DESC vertexDescription{};
        vertexDescription.ByteWidth = static_cast<UINT>(scene.vertices.size() * sizeof(Vertex));
        vertexDescription.Usage = D3D11_USAGE_IMMUTABLE;
        vertexDescription.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vertexData{};
        vertexData.pSysMem = scene.vertices.data();
        Check(device_->CreateBuffer(&vertexDescription, &vertexData, &vertexBuffer_), "Create vertex buffer failed");

        D3D11_BUFFER_DESC indexDescription{};
        indexDescription.ByteWidth = static_cast<UINT>(scene.indices.size() * sizeof(uint32_t));
        indexDescription.Usage = D3D11_USAGE_IMMUTABLE;
        indexDescription.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA indexData{};
        indexData.pSysMem = scene.indices.data();
        Check(device_->CreateBuffer(&indexDescription, &indexData, &indexBuffer_), "Create index buffer failed");

        TgaImage white;
        white.width = 1;
        white.height = 1;
        white.pixels = {255, 255, 255, 255};
        whiteTexture_ = CreateTexture(device_.Get(), context_.Get(), white);

        for (size_t i = 0; i < scene.materials.size() && gRunning; ++i)
        {
            MaterialData& material = scene.materials[i];
            if (!material.texturePath.empty())
            {
                try
                {
                    material.texture = CreateTexture(device_.Get(), context_.Get(), LoadTga(basePath / material.texturePath));
                }
                catch (const std::exception& error)
                {
                    std::cerr << error.what() << '\n';
                }
            }
            PumpMessages();
        }
    }

    void Resize(uint32_t width, uint32_t height)
    {
        if (!swapChain_ || width == 0 || height == 0)
            return;
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        renderTarget_.Reset();
        depthView_.Reset();
        Check(swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers failed");

        ComPtr<ID3D11Texture2D> backBuffer;
        Check(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)), "GetBuffer failed");
        Check(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTarget_), "CreateRenderTargetView failed");

        D3D11_TEXTURE2D_DESC depthDescription{};
        depthDescription.Width = width;
        depthDescription.Height = height;
        depthDescription.MipLevels = 1;
        depthDescription.ArraySize = 1;
        depthDescription.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthDescription.SampleDesc.Count = 1;
        depthDescription.Usage = D3D11_USAGE_DEFAULT;
        depthDescription.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> depth;
        Check(device_->CreateTexture2D(&depthDescription, nullptr, &depth), "Create depth buffer failed");
        Check(device_->CreateDepthStencilView(depth.Get(), nullptr, &depthView_), "Create depth view failed");

        viewport_ = {0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
        width_ = width;
        height_ = height;
    }

    void Draw(const SceneData& scene, const Constants& constants)
    {
        const float background[4] = {0.035f, 0.045f, 0.065f, 1.0f};
        context_->ClearRenderTargetView(renderTarget_.Get(), background);
        context_->ClearDepthStencilView(depthView_.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        ID3D11RenderTargetView* target = renderTarget_.Get();
        context_->OMSetRenderTargets(1, &target, depthView_.Get());
        context_->RSSetViewports(1, &viewport_);

        UINT stride = sizeof(Vertex);
        UINT offset = 0;
        ID3D11Buffer* vertexBuffer = vertexBuffer_.Get();
        context_->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
        context_->IASetIndexBuffer(indexBuffer_.Get(), DXGI_FORMAT_R32_UINT, 0);
        context_->IASetInputLayout(inputLayout_.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
        ID3D11SamplerState* sampler = sampler_.Get();
        context_->PSSetSamplers(0, 1, &sampler);
        context_->RSSetState(rasterizer_.Get());

        for (const MaterialData& material : scene.materials)
        {
            if (material.indexCount == 0)
                continue;
            Constants materialConstants = constants;
            materialConstants.diffuseColor = material.color;
            context_->UpdateSubresource(constantsBuffer_.Get(), 0, nullptr, &materialConstants, 0, 0);
            ID3D11Buffer* buffer = constantsBuffer_.Get();
            context_->VSSetConstantBuffers(0, 1, &buffer);
            context_->PSSetConstantBuffers(0, 1, &buffer);
            ID3D11ShaderResourceView* texture = material.texture ? material.texture.Get() : whiteTexture_.Get();
            context_->PSSetShaderResources(0, 1, &texture);
            context_->DrawIndexed(material.indexCount, material.indexStart, 0);
        }

        Check(swapChain_->Present(1, 0), "Present failed");
    }

    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }

private:
    void CreatePipeline()
    {
        static const char* shaderSource = R"(
cbuffer SceneConstants : register(b0)
{
    row_major float4x4 WorldViewProjection;
    row_major float4x4 World;
    float4 TilingOffset;
    float4 LightPosition;
    float4 CameraPosition;
    float4 DiffuseColor;
};
Texture2D DiffuseTexture : register(t0);
SamplerState TextureSampler : register(s0);
struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 texcoord : TEXCOORD0;
};
struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : POSITION0;
    float3 normal : NORMAL;
    float2 texcoord : TEXCOORD0;
};
PSInput VSMain(VSInput input)
{
    PSInput output;
    output.position = mul(float4(input.position, 1.0), WorldViewProjection);
    output.worldPosition = mul(float4(input.position, 1.0), World).xyz;
    output.normal = normalize(mul(float4(input.normal, 0.0), World).xyz);
    output.texcoord = input.texcoord * TilingOffset.xy + TilingOffset.zw;
    return output;
}
float4 PSMain(PSInput input, bool frontFace : SV_IsFrontFace) : SV_TARGET
{
    float4 surface = DiffuseTexture.Sample(TextureSampler, input.texcoord) * DiffuseColor;
    clip(surface.a - 0.05);
    float3 normal = normalize(input.normal) * (frontFace ? 1.0 : -1.0);
    float3 light = normalize(LightPosition.xyz - input.worldPosition);
    float3 view = normalize(CameraPosition.xyz - input.worldPosition);
    float3 halfVector = normalize(light + view);
    float diffuse = max(dot(normal, light), 0.0);
    float specular = pow(max(dot(normal, halfVector), 0.0), 24.0);
    float3 color = surface.rgb * (0.28 + 0.72 * diffuse) + specular * 0.12;
    return float4(color, surface.a);
}
)";

        ComPtr<ID3DBlob> vertexCode = CompileShader(shaderSource, "VSMain", "vs_5_0");
        ComPtr<ID3DBlob> pixelCode = CompileShader(shaderSource, "PSMain", "ps_5_0");
        Check(device_->CreateVertexShader(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr, &vertexShader_), "CreateVertexShader failed");
        Check(device_->CreatePixelShader(pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(), nullptr, &pixelShader_), "CreatePixelShader failed");

        D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, normal), D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, texcoord), D3D11_INPUT_PER_VERTEX_DATA, 0}
        };
        Check(device_->CreateInputLayout(elements, 3, vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), &inputLayout_), "CreateInputLayout failed");

        D3D11_BUFFER_DESC constantsDescription{};
        constantsDescription.ByteWidth = sizeof(Constants);
        constantsDescription.Usage = D3D11_USAGE_DEFAULT;
        constantsDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Check(device_->CreateBuffer(&constantsDescription, nullptr, &constantsBuffer_), "Create constant buffer failed");

        D3D11_SAMPLER_DESC samplerDescription{};
        samplerDescription.Filter = D3D11_FILTER_ANISOTROPIC;
        samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDescription.MaxAnisotropy = 8;
        samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;
        Check(device_->CreateSamplerState(&samplerDescription, &sampler_), "CreateSamplerState failed");

        D3D11_RASTERIZER_DESC rasterizerDescription{};
        rasterizerDescription.FillMode = D3D11_FILL_SOLID;
        rasterizerDescription.CullMode = D3D11_CULL_NONE;
        rasterizerDescription.DepthClipEnable = TRUE;
        Check(device_->CreateRasterizerState(&rasterizerDescription, &rasterizer_), "CreateRasterizerState failed");
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain> swapChain_;
    ComPtr<ID3D11RenderTargetView> renderTarget_;
    ComPtr<ID3D11DepthStencilView> depthView_;
    ComPtr<ID3D11Buffer> vertexBuffer_;
    ComPtr<ID3D11Buffer> indexBuffer_;
    ComPtr<ID3D11Buffer> constantsBuffer_;
    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader> pixelShader_;
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11RasterizerState> rasterizer_;
    ComPtr<ID3D11ShaderResourceView> whiteTexture_;
    D3D11_VIEWPORT viewport_{};
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

int main(int argc, char** argv)
{
    try
    {
        bool testMode = argc > 1 && std::string(argv[1]) == "--test";
        std::filesystem::path modelPath = testMode ? "sponza.obj" : (argc > 1 ? argv[1] : "sponza.obj");
        modelPath = std::filesystem::absolute(modelPath);
        if (!std::filesystem::exists(modelPath))
            throw std::runtime_error("sponza.obj not found in the program working directory");

        HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = WindowProcedure;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        windowClass.lpszClassName = L"CGTextureLabWindow";
        Check(RegisterClassExW(&windowClass) ? S_OK : HRESULT_FROM_WIN32(GetLastError()), "RegisterClassEx failed");

        RECT rectangle = {0, 0, static_cast<LONG>(gClientWidth), static_cast<LONG>(gClientHeight)};
        AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE);
        HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"OBJ materials, texture animation and tiling",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
            nullptr, nullptr, instance, nullptr);
        if (!window)
            throw std::runtime_error("CreateWindowEx failed");
        ShowWindow(window, testMode ? SW_HIDE : SW_SHOW);
        UpdateWindow(window);

        Renderer renderer;
        renderer.Initialize(window, gClientWidth, gClientHeight);
        SetWindowTextW(window, L"Loading Sponza...");
        SceneData scene = LoadObj(modelPath);
        if (!gRunning)
            return 0;
        renderer.Upload(scene, modelPath.parent_path());
        SetWindowTextW(window, L"OBJ materials, texture animation and tiling");

        XMFLOAT3 center = {
            (scene.minimum.x + scene.maximum.x) * 0.5f,
            (scene.minimum.y + scene.maximum.y) * 0.5f,
            (scene.minimum.z + scene.maximum.z) * 0.5f
        };
        float extent = std::max({scene.maximum.x - scene.minimum.x, scene.maximum.y - scene.minimum.y, scene.maximum.z - scene.minimum.z});
        float scale = 2.0f / extent;
        XMMATRIX world = XMMatrixTranslation(-center.x, -center.y, -center.z) * XMMatrixScaling(scale, scale, scale);

        auto previous = std::chrono::steady_clock::now();
        float textureTime = 0.0f;
        uint32_t frameCount = 0;
        bool firstFrameReported = false;

        while (gRunning)
        {
            PumpMessages();
            if (!gRunning)
                break;
            if (gResizePending)
            {
                renderer.Resize(gClientWidth, gClientHeight);
                gResizePending = false;
            }

            auto current = std::chrono::steady_clock::now();
            float delta = std::chrono::duration<float>(current - previous).count();
            previous = current;
            if (gAnimation)
                textureTime = std::fmod(textureTime + delta * 0.12f, 1.0f);
            if (gWheel != 0.0f)
            {
                gDistance *= std::exp(-gWheel * 0.12f);
                gDistance = std::clamp(gDistance, 0.35f, 7.0f);
                gWheel = 0.0f;
            }

            XMFLOAT3 cameraPosition = {
                gDistance * std::cos(gPitch) * std::sin(gYaw),
                gDistance * std::sin(gPitch),
                gDistance * std::cos(gPitch) * std::cos(gYaw)
            };
            XMMATRIX view = XMMatrixLookAtLH(XMLoadFloat3(&cameraPosition), XMVectorZero(), XMVectorSet(0, 1, 0, 0));
            float aspect = static_cast<float>(renderer.Width()) / renderer.Height();
            XMMATRIX projection = XMMatrixPerspectiveFovLH(XMConvertToRadians(55.0f), aspect, 0.02f, 100.0f);

            Constants constants{};
            XMStoreFloat4x4(&constants.world, world);
            XMStoreFloat4x4(&constants.worldViewProjection, world * view * projection);
            constants.tilingOffset = {gTiling, gTiling, textureTime, textureTime * 0.55f};
            constants.lightPosition = {2.5f, 3.5f, 2.0f, 1.0f};
            constants.cameraPosition = {cameraPosition.x, cameraPosition.y, cameraPosition.z, 1.0f};
            constants.diffuseColor = {1, 1, 1, 1};
            renderer.Draw(scene, constants);

            if (!firstFrameReported)
            {
                std::cout << "READY vertices=" << scene.vertices.size() << " triangles=" << scene.indices.size() / 3 << '\n';
                firstFrameReported = true;
            }
            if (testMode && ++frameCount >= 30)
                break;
        }

        DestroyWindow(window);
        UnregisterClassW(windowClass.lpszClassName, instance);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        MessageBoxA(nullptr, error.what(), "CG laboratory error", MB_OK | MB_ICONERROR);
        return 1;
    }
}
