#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX;
namespace fs = std::filesystem;

constexpr int kCascades = 4;
constexpr int kShadowSize = 2048;
constexpr float kNear = 0.1f;
constexpr float kFar = 75.0f;
constexpr float kFov = XM_PIDIV4 * 1.6f;
constexpr float kSplitLambda = 0.75f;

void check(HRESULT hr, const char* where)
{
    if (FAILED(hr)) throw std::runtime_error(std::string(where) + " failed: 0x" + std::to_string(static_cast<unsigned>(hr)));
}

struct Vertex { XMFLOAT3 position, normal; XMFLOAT2 uv; };
struct Batch { uint32_t start = 0, count = 0; std::string material; };
struct Material {
    std::string texture;
    ComPtr<ID3D11ShaderResourceView> srv;
};

struct alignas(16) SceneCB {
    XMFLOAT4X4 viewProjection;
    XMFLOAT4X4 lightViewProjection[kCascades];
    XMFLOAT4 cascadeEnds;
    XMFLOAT4 lightDirection;
    XMFLOAT4 eyePosition;
    XMFLOAT4 cameraForward;
    XMFLOAT4 options;
};
struct alignas(16) ShadowCB { XMFLOAT4X4 viewProjection; };

std::string trim(const std::string& s)
{
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::unordered_map<std::string, Material> loadMtl(const fs::path& path)
{
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Cannot open " + path.string());
    std::unordered_map<std::string, Material> materials;
    std::string line, name;
    while (std::getline(in, line)) {
        if (line.rfind("newmtl ", 0) == 0) {
            name = trim(line.substr(7));
            materials.try_emplace(name);
        } else if (line.rfind("map_Kd ", 0) == 0 && !name.empty()) {
            materials[name].texture = trim(line.substr(7));
        }
    }
    return materials;
}

int resolveIndex(int index, size_t count)
{
    if (index > 0) return index - 1;
    if (index < 0) return static_cast<int>(count) + index;
    return -1;
}

struct FaceIndex { int v = 0, t = 0, n = 0; };
FaceIndex parseFaceIndex(const std::string& token)
{
    FaceIndex result;
    sscanf_s(token.c_str(), "%d/%d/%d", &result.v, &result.t, &result.n);
    return result;
}

void loadObj(const fs::path& path, std::vector<Vertex>& vertices, std::vector<Batch>& batches)
{
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Cannot open " + path.string());
    std::vector<XMFLOAT3> positions, normals;
    std::vector<XMFLOAT2> uvs;
    std::string line, material;
    while (std::getline(in, line)) {
        if (line.rfind("v ", 0) == 0) {
            XMFLOAT3 p{}; sscanf_s(line.c_str() + 2, "%f %f %f", &p.x, &p.y, &p.z); positions.push_back(p);
        } else if (line.rfind("vt ", 0) == 0) {
            XMFLOAT2 uv{}; sscanf_s(line.c_str() + 3, "%f %f", &uv.x, &uv.y); uvs.push_back(uv);
        } else if (line.rfind("vn ", 0) == 0) {
            XMFLOAT3 n{}; sscanf_s(line.c_str() + 3, "%f %f %f", &n.x, &n.y, &n.z); normals.push_back(n);
        } else if (line.rfind("usemtl ", 0) == 0) {
            material = trim(line.substr(7));
            batches.push_back({static_cast<uint32_t>(vertices.size()), 0, material});
        } else if (line.rfind("f ", 0) == 0) {
            if (batches.empty()) batches.push_back({static_cast<uint32_t>(vertices.size()), 0, material});
            std::istringstream stream(line.substr(2));
            std::vector<FaceIndex> face;
            std::string token;
            while (stream >> token) face.push_back(parseFaceIndex(token));
            auto emit = [&](const FaceIndex& i) {
                const int v = resolveIndex(i.v, positions.size());
                const int t = resolveIndex(i.t, uvs.size());
                const int n = resolveIndex(i.n, normals.size());
                if (v < 0 || v >= static_cast<int>(positions.size())) throw std::runtime_error("Bad OBJ position index");
                vertices.push_back({positions[v], n >= 0 && n < static_cast<int>(normals.size()) ? normals[n] : XMFLOAT3{0, 1, 0},
                    t >= 0 && t < static_cast<int>(uvs.size()) ? uvs[t] : XMFLOAT2{0, 0}});
                ++batches.back().count;
            };
            for (size_t i = 1; i + 1 < face.size(); ++i) { emit(face[0]); emit(face[i]); emit(face[i + 1]); }
        }
    }
    batches.erase(std::remove_if(batches.begin(), batches.end(), [](const Batch& b) { return b.count == 0; }), batches.end());
    if (vertices.empty()) throw std::runtime_error("OBJ contains no triangles");
}

std::vector<uint8_t> loadTga(const fs::path& path, UINT& width, UINT& height)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open " + path.string());
    std::array<uint8_t, 18> header{};
    in.read(reinterpret_cast<char*>(header.data()), header.size());
    width = UINT(header[12]) | (UINT(header[13]) << 8);
    height = UINT(header[14]) | (UINT(header[15]) << 8);
    const int bytes = header[16] / 8;
    if (!in || header[1] != 0 || header[2] != 2 || (bytes != 3 && bytes != 4) || !width || !height)
        throw std::runtime_error("Unsupported TGA: " + path.string());
    in.seekg(header[0], std::ios::cur);
    std::vector<uint8_t> source(size_t(width) * height * bytes), rgba(size_t(width) * height * 4);
    in.read(reinterpret_cast<char*>(source.data()), static_cast<std::streamsize>(source.size()));
    if (!in) throw std::runtime_error("Truncated TGA: " + path.string());
    const bool topOrigin = (header[17] & 0x20) != 0;
    for (UINT y = 0; y < height; ++y) {
        const UINT sourceY = topOrigin ? y : height - 1 - y;
        for (UINT x = 0; x < width; ++x) {
            const size_t s = (size_t(sourceY) * width + x) * bytes;
            const size_t d = (size_t(y) * width + x) * 4;
            rgba[d] = source[s + 2]; rgba[d + 1] = source[s + 1]; rgba[d + 2] = source[s];
            rgba[d + 3] = bytes == 4 ? source[s + 3] : 255;
        }
    }
    return rgba;
}

XMVECTOR point(float x, float y, float z) { return XMVectorSet(x, y, z, 1); }
XMVECTOR direction(float x, float y, float z) { return XMVectorSet(x, y, z, 0); }
XMFLOAT4 asFloat4(FXMVECTOR v) { XMFLOAT4 out; XMStoreFloat4(&out, v); return out; }

class App {
public:
    explicit App(const fs::path& root) : root_(root) { createWindow(); createDevice(); createPipeline(); createScene(); }
    int run();
    int verify(bool hardShadows) { pcf_ = !hardShadows; updateWindowTitle(); update(0.0f); render(true); return 0; }
    void resize(UINT width, UINT height);
    HWND hwnd() const { return hwnd_; }
private:
    void createWindow();
    void createDevice();
    void createBackbuffer(UINT width, UINT height);
    void createPipeline();
    void createScene();
    void createShadows();
    void updateWindowTitle();
    void update(float dt);
    void render(bool capture = false);
    void captureFrame();
    void drawScene();
    ComPtr<ID3D11ShaderResourceView> makeTexture(const fs::path& path);
    fs::path root_;
    HWND hwnd_ = nullptr;
    UINT width_ = 1280, height_ = 720;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain> swapChain_;
    ComPtr<ID3D11RenderTargetView> backbuffer_;
    ComPtr<ID3D11Texture2D> depthTexture_, shadowTexture_;
    ComPtr<ID3D11DepthStencilView> depthView_;
    std::array<ComPtr<ID3D11DepthStencilView>, kCascades> shadowViews_;
    ComPtr<ID3D11ShaderResourceView> shadowSrv_, whiteSrv_;
    ComPtr<ID3D11VertexShader> sceneVS_, shadowVS_;
    ComPtr<ID3D11PixelShader> scenePS_, shadowPS_;
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11SamplerState> diffuseSampler_, shadowSampler_;
    ComPtr<ID3D11RasterizerState> sceneRaster_, shadowRaster_;
    ComPtr<ID3D11Buffer> vertexBuffer_, sceneCB_, shadowCB_;
    std::vector<Batch> batches_;
    std::unordered_map<std::string, Material> materials_;
    std::array<float, kCascades> splits_{};
    SceneCB sceneData_{};
    XMFLOAT3 eye_{0, 5, 0};
    float yaw_ = XM_PIDIV2, pitch_ = 0;
    bool cascadeColors_ = false, pcf_ = true;
    bool oldC_ = false, oldP_ = false;
};

LRESULT CALLBACK wndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (message == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
    if (message == WM_SIZE) {
        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (app && wParam != SIZE_MINIMIZED) app->resize(LOWORD(lParam), HIWORD(lParam));
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void App::createWindow()
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SponzaCSMWindow"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    check(RegisterClassW(&wc) ? S_OK : HRESULT_FROM_WIN32(GetLastError()), "RegisterClassW");
    RECT rect{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd_ = CreateWindowW(wc.lpszClassName, L"Sponza CSM | PCF: ON | C: cascade colors | P / russian Pe: PCF",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd_) throw std::runtime_error("CreateWindowW failed");
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    ShowWindow(hwnd_, SW_SHOW);
}

void App::createDevice()
{
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.Width = width_; desc.BufferDesc.Height = height_;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2; desc.OutputWindow = hwnd_; desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL feature{};
    D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        requested, 1, D3D11_SDK_VERSION, &desc, swapChain_.GetAddressOf(), device_.GetAddressOf(),
        &feature, context_.GetAddressOf());
    if (FAILED(hr)) {
        check(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            requested, 1, D3D11_SDK_VERSION, &desc, swapChain_.GetAddressOf(), device_.GetAddressOf(),
            &feature, context_.GetAddressOf()), "D3D11CreateDeviceAndSwapChain");
    }
    createBackbuffer(width_, height_);
    createShadows();
}

void App::createBackbuffer(UINT width, UINT height)
{
    ComPtr<ID3D11Texture2D> back;
    check(swapChain_->GetBuffer(0, IID_PPV_ARGS(back.GetAddressOf())), "GetBuffer");
    check(device_->CreateRenderTargetView(back.Get(), nullptr, backbuffer_.GetAddressOf()), "CreateRenderTargetView");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    check(device_->CreateTexture2D(&desc, nullptr, depthTexture_.GetAddressOf()), "Create depth texture");
    check(device_->CreateDepthStencilView(depthTexture_.Get(), nullptr, depthView_.GetAddressOf()), "Create depth view");
}

void App::resize(UINT width, UINT height)
{
    if (!swapChain_ || width == 0 || height == 0) return;
    width_ = width; height_ = height;
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    backbuffer_.Reset(); depthView_.Reset(); depthTexture_.Reset();
    check(swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers");
    createBackbuffer(width, height);
}

ComPtr<ID3DBlob> compileShader(const fs::path& path, const char* entry, const char* target)
{
    ComPtr<ID3DBlob> blob, errors;
    HRESULT hr = D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0, blob.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr)) {
        const char* detail = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "Unknown shader error";
        throw std::runtime_error(std::string(entry) + ": " + detail);
    }
    return blob;
}

void App::createPipeline()
{
    const auto shader = root_ / "shaders" / "scene.hlsl";
    auto sv = compileShader(shader, "SceneVS", "vs_5_0");
    auto sp = compileShader(shader, "ScenePS", "ps_5_0");
    auto dv = compileShader(shader, "ShadowVS", "vs_5_0");
    auto dp = compileShader(shader, "ShadowPS", "ps_5_0");
    check(device_->CreateVertexShader(sv->GetBufferPointer(), sv->GetBufferSize(), nullptr, sceneVS_.GetAddressOf()), "SceneVS");
    check(device_->CreatePixelShader(sp->GetBufferPointer(), sp->GetBufferSize(), nullptr, scenePS_.GetAddressOf()), "ScenePS");
    check(device_->CreateVertexShader(dv->GetBufferPointer(), dv->GetBufferSize(), nullptr, shadowVS_.GetAddressOf()), "ShadowVS");
    check(device_->CreatePixelShader(dp->GetBufferPointer(), dp->GetBufferSize(), nullptr, shadowPS_.GetAddressOf()), "ShadowPS");
    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0}
    };
    check(device_->CreateInputLayout(layout, 3, sv->GetBufferPointer(), sv->GetBufferSize(), inputLayout_.GetAddressOf()), "CreateInputLayout");
    D3D11_BUFFER_DESC cb{}; cb.ByteWidth = sizeof(SceneCB); cb.Usage = D3D11_USAGE_DEFAULT; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    check(device_->CreateBuffer(&cb, nullptr, sceneCB_.GetAddressOf()), "Create SceneCB");
    cb.ByteWidth = sizeof(ShadowCB);
    check(device_->CreateBuffer(&cb, nullptr, shadowCB_.GetAddressOf()), "Create ShadowCB");
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    check(device_->CreateSamplerState(&sampler, diffuseSampler_.GetAddressOf()), "Create diffuse sampler");
    sampler.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    sampler.BorderColor[0] = sampler.BorderColor[1] = sampler.BorderColor[2] = sampler.BorderColor[3] = 1;
    sampler.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    check(device_->CreateSamplerState(&sampler, shadowSampler_.GetAddressOf()), "Create shadow sampler");
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE; raster.DepthClipEnable = TRUE;
    check(device_->CreateRasterizerState(&raster, sceneRaster_.GetAddressOf()), "Create scene rasterizer");
    raster.DepthBias = 250; raster.SlopeScaledDepthBias = 1.25f;
    check(device_->CreateRasterizerState(&raster, shadowRaster_.GetAddressOf()), "Create shadow rasterizer");
}

ComPtr<ID3D11ShaderResourceView> App::makeTexture(const fs::path& path)
{
    UINT width = 0, height = 0;
    const auto data = loadTga(path, width, height);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{}; initial.pSysMem = data.data(); initial.SysMemPitch = width * 4;
    ComPtr<ID3D11Texture2D> texture;
    check(device_->CreateTexture2D(&desc, &initial, texture.GetAddressOf()), "Create texture");
    ComPtr<ID3D11ShaderResourceView> srv;
    check(device_->CreateShaderResourceView(texture.Get(), nullptr, srv.GetAddressOf()), "Create texture view");
    return srv;
}

void App::createScene()
{
    const auto model = root_ / "assets" / "Sponza-master";
    materials_ = loadMtl(model / "sponza.mtl");
    std::vector<Vertex> vertices;
    loadObj(model / "sponza.obj", vertices, batches_);
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(Vertex));
    desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{}; initial.pSysMem = vertices.data();
    check(device_->CreateBuffer(&desc, &initial, vertexBuffer_.GetAddressOf()), "Create vertex buffer");
    // White texture for the untextured OBJ material.
    uint32_t white = 0xffffffff;
    D3D11_TEXTURE2D_DESC td{}; td.Width = td.Height = td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA pixel{}; pixel.pSysMem = &white; pixel.SysMemPitch = 4;
    ComPtr<ID3D11Texture2D> texture;
    check(device_->CreateTexture2D(&td, &pixel, texture.GetAddressOf()), "Create white texture");
    check(device_->CreateShaderResourceView(texture.Get(), nullptr, whiteSrv_.GetAddressOf()), "Create white SRV");
    std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>> loaded;
    for (auto& [name, material] : materials_) {
        if (material.texture.empty()) { material.srv = whiteSrv_; continue; }
        const fs::path texPath = model / fs::path(material.texture);
        if (!fs::exists(texPath)) { material.srv = whiteSrv_; continue; }
        auto [it, inserted] = loaded.try_emplace(material.texture);
        if (inserted) it->second = makeTexture(texPath);
        material.srv = it->second;
    }
}

void App::createShadows()
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = kShadowSize; desc.MipLevels = 1; desc.ArraySize = kCascades;
    desc.Format = DXGI_FORMAT_R32_TYPELESS; desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    check(device_->CreateTexture2D(&desc, nullptr, shadowTexture_.GetAddressOf()), "Create shadow array");
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
    dsv.Texture2DArray.ArraySize = 1;
    for (int i = 0; i < kCascades; ++i) {
        dsv.Texture2DArray.FirstArraySlice = i;
        check(device_->CreateDepthStencilView(shadowTexture_.Get(), &dsv, shadowViews_[i].GetAddressOf()), "Create cascade DSV");
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Texture2DArray.ArraySize = kCascades;
    srv.Texture2DArray.MipLevels = 1;
    check(device_->CreateShaderResourceView(shadowTexture_.Get(), &srv, shadowSrv_.GetAddressOf()), "Create shadow SRV");
}

void App::update(float dt)
{
    const bool c = (GetAsyncKeyState('C') & 0x8000) != 0;
    // Russian 'П' occupies the physical G key; accept both that key and P.
    const bool p = ((GetAsyncKeyState('P') | GetAsyncKeyState('G')) & 0x8000) != 0;
    bool titleChanged = false;
    if (c && !oldC_) { cascadeColors_ = !cascadeColors_; titleChanged = true; }
    if (p && !oldP_) { pcf_ = !pcf_; titleChanged = true; }
    if (titleChanged) updateWindowTitle();
    oldC_ = c; oldP_ = p;
    const float turn = 1.45f * dt;
    if (GetAsyncKeyState(VK_LEFT) & 0x8000) yaw_ -= turn;
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) yaw_ += turn;
    if (GetAsyncKeyState(VK_UP) & 0x8000) pitch_ += turn;
    if (GetAsyncKeyState(VK_DOWN) & 0x8000) pitch_ -= turn;
    pitch_ = std::clamp(pitch_, -1.3f, 1.3f);
    const XMVECTOR forward = XMVector3Normalize(direction(std::sin(yaw_) * std::cos(pitch_),
        std::sin(pitch_), std::cos(yaw_) * std::cos(pitch_)));
    const XMVECTOR right = XMVector3Normalize(XMVector3Cross(direction(0, 1, 0), forward));
    XMVECTOR velocity = XMVectorZero();
    if (GetAsyncKeyState('W') & 0x8000) velocity += forward;
    if (GetAsyncKeyState('S') & 0x8000) velocity -= forward;
    if (GetAsyncKeyState('D') & 0x8000) velocity += right;
    if (GetAsyncKeyState('A') & 0x8000) velocity -= right;
    if (GetAsyncKeyState('E') & 0x8000) velocity += direction(0, 1, 0);
    if (GetAsyncKeyState('Q') & 0x8000) velocity -= direction(0, 1, 0);
    if (XMVectorGetX(XMVector3LengthSq(velocity)) > 0.0001f) {
        const float speed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 17.0f : 6.0f;
        XMVECTOR eye = XMLoadFloat3(&eye_) + XMVector3Normalize(velocity) * (speed * dt);
        XMStoreFloat3(&eye_, eye);
    }
    const XMVECTOR eye = XMLoadFloat3(&eye_);
    const XMVECTOR up = direction(0, 1, 0);
    const XMMATRIX view = XMMatrixLookToLH(eye, forward, up);
    const float aspect = float(width_) / float(height_);
    const XMMATRIX projection = XMMatrixPerspectiveFovLH(kFov, aspect, kNear, kFar);
    XMStoreFloat4x4(&sceneData_.viewProjection, view * projection);
    const XMVECTOR lightDir = XMVector3Normalize(direction(-0.55f, -1.0f, 0.30f));
    sceneData_.lightDirection = asFloat4(lightDir);
    sceneData_.eyePosition = asFloat4(eye);
    sceneData_.cameraForward = asFloat4(forward);
    sceneData_.options = XMFLOAT4(cascadeColors_ ? 1.0f : 0.0f, pcf_ ? 1.0f : 0.0f, float(kShadowSize), 0);

    const XMVECTOR cameraRight = XMVector3Normalize(XMVector3Cross(up, forward));
    const XMVECTOR cameraUp = XMVector3Normalize(XMVector3Cross(forward, cameraRight));
    const float tangent = std::tan(kFov * 0.5f);
    float sliceNear = kNear;
    for (int cascade = 0; cascade < kCascades; ++cascade) {
        const float fraction = float(cascade + 1) / kCascades;
        const float logSplit = kNear * std::pow(kFar / kNear, fraction);
        const float uniformSplit = kNear + (kFar - kNear) * fraction;
        const float sliceFar = kSplitLambda * logSplit + (1.0f - kSplitLambda) * uniformSplit;
        splits_[cascade] = sliceFar;
        std::array<XMVECTOR, 8> corners;
        int index = 0;
        for (float depth : {sliceNear, sliceFar}) {
            XMVECTOR center = eye + forward * depth;
            const float halfHeight = depth * tangent;
            const float halfWidth = halfHeight * aspect;
            for (int y : {-1, 1}) for (int x : {-1, 1})
                corners[index++] = center + cameraRight * (float(x) * halfWidth) + cameraUp * (float(y) * halfHeight);
        }
        XMVECTOR center = XMVectorZero();
        for (const XMVECTOR& corner : corners) center += corner;
        center /= 8.0f;
        const XMMATRIX lightView = XMMatrixLookToLH(center - lightDir * 100.0f, lightDir, up);
        float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f, minZ = 1e9f, maxZ = -1e9f;
        for (const XMVECTOR& corner : corners) {
            const XMFLOAT4 transformed = asFloat4(XMVector3TransformCoord(corner, lightView));
            minX = std::min(minX, transformed.x); maxX = std::max(maxX, transformed.x);
            minY = std::min(minY, transformed.y); maxY = std::max(maxY, transformed.y);
            minZ = std::min(minZ, transformed.z); maxZ = std::max(maxZ, transformed.z);
        }
        // A square projection and texel snapping reduce shimmering while the camera moves.
        const float extent = std::max(maxX - minX, maxY - minY) * 1.05f;
        const float texel = extent / float(kShadowSize);
        const float centerX = std::floor(((minX + maxX) * 0.5f) / texel) * texel;
        const float centerY = std::floor(((minY + maxY) * 0.5f) / texel) * texel;
        const XMMATRIX lightProjection = XMMatrixOrthographicOffCenterLH(
            centerX - extent * 0.5f, centerX + extent * 0.5f,
            centerY - extent * 0.5f, centerY + extent * 0.5f,
            std::max(0.01f, minZ - 55.0f), maxZ + 55.0f);
        XMStoreFloat4x4(&sceneData_.lightViewProjection[cascade], lightView * lightProjection);
        sliceNear = sliceFar;
    }
    sceneData_.cascadeEnds = XMFLOAT4(splits_[0], splits_[1], splits_[2], splits_[3]);
    context_->UpdateSubresource(sceneCB_.Get(), 0, nullptr, &sceneData_, 0, 0);
}

void App::updateWindowTitle()
{
    std::wstring title = L"Sponza CSM | PCF: ";
    title += pcf_ ? L"ON" : L"OFF";
    title += L" | Cascade colors: ";
    title += cascadeColors_ ? L"ON" : L"OFF";
    title += L" | P / П: PCF, C: cascades";
    SetWindowTextW(hwnd_, title.c_str());
}

void App::drawScene()
{
    for (const Batch& batch : batches_) {
        auto it = materials_.find(batch.material);
        ID3D11ShaderResourceView* texture = it != materials_.end() && it->second.srv ? it->second.srv.Get() : whiteSrv_.Get();
        context_->PSSetShaderResources(0, 1, &texture);
        context_->Draw(batch.count, batch.start);
    }
}

void App::captureFrame()
{
    ComPtr<ID3D11Texture2D> back;
    check(swapChain_->GetBuffer(0, IID_PPV_ARGS(back.GetAddressOf())), "GetBuffer for capture");
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    check(device_->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()), "Create capture texture");
    context_->CopyResource(staging.Get(), back.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map capture texture");
    std::ofstream out(root_ / "preview.ppm", std::ios::binary);
    out << "P6\n" << width_ << ' ' << height_ << "\n255\n";
    for (UINT y = 0; y < height_; ++y) {
        const auto* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
        for (UINT x = 0; x < width_; ++x) out.write(reinterpret_cast<const char*>(row + size_t(x) * 4), 3);
    }
    context_->Unmap(staging.Get(), 0);
    if (!out) throw std::runtime_error("Cannot save preview.ppm");
}

void App::render(bool capture)
{
    UINT stride = sizeof(Vertex), offset = 0;
    ID3D11Buffer* vb = vertexBuffer_.Get();
    context_->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    context_->IASetInputLayout(inputLayout_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11SamplerState* sampler = diffuseSampler_.Get();
    context_->PSSetSamplers(0, 1, &sampler);
    ID3D11Buffer* shadowCB = shadowCB_.Get();
    context_->VSSetConstantBuffers(1, 1, &shadowCB);
    context_->VSSetShader(shadowVS_.Get(), nullptr, 0);
    context_->PSSetShader(shadowPS_.Get(), nullptr, 0);
    context_->RSSetState(shadowRaster_.Get());
    D3D11_VIEWPORT shadowViewport{0, 0, float(kShadowSize), float(kShadowSize), 0, 1};
    context_->RSSetViewports(1, &shadowViewport);
    // The previous frame's SRV must be unbound before writing the depth array.
    ID3D11ShaderResourceView* nullSrv = nullptr;
    context_->PSSetShaderResources(1, 1, &nullSrv);
    for (int cascade = 0; cascade < kCascades; ++cascade) {
        ShadowCB cb{sceneData_.lightViewProjection[cascade]};
        context_->UpdateSubresource(shadowCB_.Get(), 0, nullptr, &cb, 0, 0);
        context_->ClearDepthStencilView(shadowViews_[cascade].Get(), D3D11_CLEAR_DEPTH, 1, 0);
        context_->OMSetRenderTargets(0, nullptr, shadowViews_[cascade].Get());
        drawScene();
    }
    const float clear[] = {0.13f, 0.18f, 0.24f, 1.0f};
    context_->ClearRenderTargetView(backbuffer_.Get(), clear);
    context_->ClearDepthStencilView(depthView_.Get(), D3D11_CLEAR_DEPTH, 1, 0);
    ID3D11RenderTargetView* rtv = backbuffer_.Get();
    context_->OMSetRenderTargets(1, &rtv, depthView_.Get());
    D3D11_VIEWPORT viewport{0, 0, float(width_), float(height_), 0, 1};
    context_->RSSetViewports(1, &viewport);
    context_->RSSetState(sceneRaster_.Get());
    context_->VSSetShader(sceneVS_.Get(), nullptr, 0);
    context_->PSSetShader(scenePS_.Get(), nullptr, 0);
    ID3D11Buffer* sceneCB = sceneCB_.Get();
    context_->VSSetConstantBuffers(0, 1, &sceneCB);
    context_->PSSetConstantBuffers(0, 1, &sceneCB);
    ID3D11ShaderResourceView* shadows = shadowSrv_.Get();
    context_->PSSetShaderResources(1, 1, &shadows);
    ID3D11SamplerState* comparison = shadowSampler_.Get();
    context_->PSSetSamplers(1, 1, &comparison);
    drawScene();
    if (capture) captureFrame();
    check(swapChain_->Present(1, 0), "Present");
}

int App::run()
{
    MSG message{};
    LARGE_INTEGER frequency{}, previous{}, now{};
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&previous);
    while (message.message != WM_QUIT) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if (message.message == WM_QUIT) break;
        if (IsIconic(hwnd_)) { Sleep(50); continue; }
        QueryPerformanceCounter(&now);
        const float dt = std::min(0.05f, float(now.QuadPart - previous.QuadPart) / float(frequency.QuadPart));
        previous = now;
        update(dt); render();
    }
    return static_cast<int>(message.wParam);
}

fs::path findRoot()
{
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    for (fs::path path : {fs::current_path(), fs::path(exe).parent_path()}) {
        for (int i = 0; i < 6; ++i) {
            if (fs::exists(path / "shaders" / "scene.hlsl") && fs::exists(path / "assets" / "Sponza-master" / "sponza.obj"))
                return path;
            if (path == path.parent_path()) break;
            path = path.parent_path();
        }
    }
    throw std::runtime_error("Project assets not found. Run from the SponzaCSM project tree.");
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int)
{
    try {
        App app(findRoot());
        if (wcsstr(commandLine, L"--verify-hard")) return app.verify(true);
        return wcsstr(commandLine, L"--verify") ? app.verify(false) : app.run();
    }
    catch (const std::exception& e) { MessageBoxA(nullptr, e.what(), "SponzaCSM error", MB_OK | MB_ICONERROR); return 1; }
}
