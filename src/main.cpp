#include "Model.h"
#include "TextureLoader.h"

#include <DirectXMath.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace
{
    void ThrowIfFailed(HRESULT hr, const char* message)
    {
        if (FAILED(hr))
        {
            std::ostringstream stream;
            stream << message << " HRESULT=0x" << std::hex << static_cast<unsigned long>(hr);
            throw std::runtime_error(stream.str());
        }
    }

    std::filesystem::path ExecutableDirectory()
    {
        std::array<wchar_t, 32768> buffer{};
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0 || length == buffer.size())
        {
            throw std::runtime_error("Cannot determine executable directory.");
        }
        return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
    }

    ComPtr<ID3DBlob> CompileShader(const std::filesystem::path& path,
                                  const char* entryPoint,
                                  const char* target)
    {
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

        ComPtr<ID3DBlob> shader;
        ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3DCompileFromFile(
            path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entryPoint, target, flags, 0, &shader, &errors);
        if (FAILED(hr))
        {
            std::string details;
            if (errors)
            {
                details.assign(static_cast<const char*>(errors->GetBufferPointer()),
                               errors->GetBufferSize());
            }
            throw std::runtime_error("Shader compilation failed: " + path.string() + "\n" + details);
        }
        return shader;
    }

    struct alignas(16) SceneConstants
    {
        XMFLOAT4X4 world;
        XMFLOAT4X4 view;
        XMFLOAT4X4 projection;
        XMFLOAT4 cameraPosition;
        XMFLOAT4 lightPosition[4];
        XMFLOAT4 lightColor[4];
        XMFLOAT4 renderParams;
        XMFLOAT4 debugParams;
    };

    static_assert(sizeof(SceneConstants) % 16 == 0,
                  "A Direct3D constant buffer must be 16-byte aligned.");

    struct SkyVertex
    {
        XMFLOAT3 position;
    };
}

class Application
{
public:
    void Create(HINSTANCE instance);
    int Run();
    LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

private:
    void CreateDeviceAndSwapChain();
    void CreateSizeDependentResources(UINT width, UINT height);
    void Resize(UINT width, UINT height);
    void CreatePipeline();
    void LoadAssets();
    void CreateSkyboxGeometry();
    void Render(float deltaSeconds);
    void UpdateWindowTitle();

    HWND window_ = nullptr;
    UINT width_ = 1280;
    UINT height_ = 720;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain> swapChain_;
    ComPtr<ID3D11RenderTargetView> renderTarget_;
    ComPtr<ID3D11Texture2D> depthTexture_;
    ComPtr<ID3D11DepthStencilView> depthView_;

    ComPtr<ID3D11VertexShader> pbrVertexShader_;
    ComPtr<ID3D11PixelShader> pbrPixelShader_;
    ComPtr<ID3D11InputLayout> pbrInputLayout_;
    ComPtr<ID3D11VertexShader> skyVertexShader_;
    ComPtr<ID3D11PixelShader> skyPixelShader_;
    ComPtr<ID3D11InputLayout> skyInputLayout_;
    ComPtr<ID3D11Buffer> sceneConstantBuffer_;
    ComPtr<ID3D11SamplerState> materialSampler_;
    ComPtr<ID3D11SamplerState> environmentSampler_;
    ComPtr<ID3D11DepthStencilState> skyDepthState_;
    ComPtr<ID3D11RasterizerState> skyRasterizer_;
    ComPtr<ID3D11Buffer> skyVertexBuffer_;

    Model model_;
    ComPtr<ID3D11ShaderResourceView> albedoMap_;
    ComPtr<ID3D11ShaderResourceView> normalMap_;
    ComPtr<ID3D11ShaderResourceView> metallicMap_;
    ComPtr<ID3D11ShaderResourceView> roughnessMap_;
    ComPtr<ID3D11ShaderResourceView> irradianceMap_;
    ComPtr<ID3D11ShaderResourceView> prefilteredMap_;
    ComPtr<ID3D11ShaderResourceView> brdfLut_;

    bool dragging_ = false;
    POINT previousMouse_{};
    float cameraYaw_ = 0.30f;
    float cameraPitch_ = 0.12f;
    float cameraDistance_ = 6.2f;
    float modelAngle_ = -0.18f;
    float exposure_ = 1.0f;
    float normalYSign_ = -1.0f;
    bool iblEnabled_ = true;
    bool directLightEnabled_ = true;
    bool autoRotate_ = true;
    int debugMode_ = 0;
};

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    Application* app = reinterpret_cast<Application*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<Application*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }

    if (app)
    {
        return app->HandleMessage(window, message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void Application::Create(HINSTANCE instance)
{
    const wchar_t* className = L"PBRLabWindow";
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = className;
    ThrowIfFailed(RegisterClassExW(&windowClass) ? S_OK : HRESULT_FROM_WIN32(GetLastError()),
                  "Cannot register the application window class.");

    RECT bounds{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE);

    window_ = CreateWindowExW(
        0, className, L"PBR Lab", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, instance, this);
    if (!window_)
    {
        throw std::runtime_error("Cannot create the application window.");
    }

    CreateDeviceAndSwapChain();
    CreatePipeline();
    CreateSkyboxGeometry();
    LoadAssets();
    UpdateWindowTitle();

    ShowWindow(window_, SW_SHOW);
    UpdateWindow(window_);
}

void Application::CreateDeviceAndSwapChain()
{
    DXGI_SWAP_CHAIN_DESC swapDesc{};
    swapDesc.BufferDesc.Width = width_;
    swapDesc.BufferDesc.Height = height_;
    swapDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = 1;
    swapDesc.OutputWindow = window_;
    swapDesc.Windowed = TRUE;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL requestedLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selectedLevel{};

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        requestedLevels, 2, D3D11_SDK_VERSION,
        &swapDesc, &swapChain_, &device_, &selectedLevel, &context_);

    if (hr == E_INVALIDARG)
    {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            &requestedLevels[1], 1, D3D11_SDK_VERSION,
            &swapDesc, &swapChain_, &device_, &selectedLevel, &context_);
    }
    ThrowIfFailed(hr, "Cannot create a Direct3D 11 device.");
    if (selectedLevel < D3D_FEATURE_LEVEL_11_0)
    {
        throw std::runtime_error("Direct3D feature level 11.0 is required.");
    }

    CreateSizeDependentResources(width_, height_);
}

void Application::CreateSizeDependentResources(UINT width, UINT height)
{
    ComPtr<ID3D11Texture2D> backBuffer;
    ThrowIfFailed(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)),
                  "Cannot get the swap-chain back buffer.");
    ThrowIfFailed(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTarget_),
                  "Cannot create the render target.");

    D3D11_TEXTURE2D_DESC depthDesc{};
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ThrowIfFailed(device_->CreateTexture2D(&depthDesc, nullptr, &depthTexture_),
                  "Cannot create the depth buffer.");
    ThrowIfFailed(device_->CreateDepthStencilView(depthTexture_.Get(), nullptr, &depthView_),
                  "Cannot create the depth-stencil view.");
}

void Application::Resize(UINT width, UINT height)
{
    if (!swapChain_ || width == 0 || height == 0)
    {
        return;
    }

    width_ = width;
    height_ = height;
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    renderTarget_.Reset();
    depthView_.Reset();
    depthTexture_.Reset();
    ThrowIfFailed(swapChain_->ResizeBuffers(0, width_, height_, DXGI_FORMAT_UNKNOWN, 0),
                  "Cannot resize the swap chain.");
    CreateSizeDependentResources(width_, height_);
}

void Application::CreatePipeline()
{
    const std::filesystem::path shaderDirectory = ExecutableDirectory() / L"shaders";

    const ComPtr<ID3DBlob> pbrVS = CompileShader(shaderDirectory / L"PBR.hlsl", "VSMain", "vs_5_0");
    const ComPtr<ID3DBlob> pbrPS = CompileShader(shaderDirectory / L"PBR.hlsl", "PSMain", "ps_5_0");
    ThrowIfFailed(device_->CreateVertexShader(pbrVS->GetBufferPointer(), pbrVS->GetBufferSize(),
                                               nullptr, &pbrVertexShader_),
                  "Cannot create the PBR vertex shader.");
    ThrowIfFailed(device_->CreatePixelShader(pbrPS->GetBufferPointer(), pbrPS->GetBufferSize(),
                                              nullptr, &pbrPixelShader_),
                  "Cannot create the PBR pixel shader.");

    const D3D11_INPUT_ELEMENT_DESC pbrElements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, normal), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, texCoord), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, tangent), D3D11_INPUT_PER_VERTEX_DATA, 0}};
    ThrowIfFailed(device_->CreateInputLayout(
                      pbrElements, static_cast<UINT>(std::size(pbrElements)),
                      pbrVS->GetBufferPointer(), pbrVS->GetBufferSize(), &pbrInputLayout_),
                  "Cannot create the PBR input layout.");

    const ComPtr<ID3DBlob> skyVS = CompileShader(shaderDirectory / L"Skybox.hlsl", "VSMain", "vs_5_0");
    const ComPtr<ID3DBlob> skyPS = CompileShader(shaderDirectory / L"Skybox.hlsl", "PSMain", "ps_5_0");
    ThrowIfFailed(device_->CreateVertexShader(skyVS->GetBufferPointer(), skyVS->GetBufferSize(),
                                               nullptr, &skyVertexShader_),
                  "Cannot create the skybox vertex shader.");
    ThrowIfFailed(device_->CreatePixelShader(skyPS->GetBufferPointer(), skyPS->GetBufferSize(),
                                              nullptr, &skyPixelShader_),
                  "Cannot create the skybox pixel shader.");

    const D3D11_INPUT_ELEMENT_DESC skyElement = {
        "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
        D3D11_INPUT_PER_VERTEX_DATA, 0};
    ThrowIfFailed(device_->CreateInputLayout(
                      &skyElement, 1, skyVS->GetBufferPointer(), skyVS->GetBufferSize(),
                      &skyInputLayout_),
                  "Cannot create the skybox input layout.");

    D3D11_BUFFER_DESC constantDesc{};
    constantDesc.ByteWidth = sizeof(SceneConstants);
    constantDesc.Usage = D3D11_USAGE_DEFAULT;
    constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ThrowIfFailed(device_->CreateBuffer(&constantDesc, nullptr, &sceneConstantBuffer_),
                  "Cannot create the scene constant buffer.");

    D3D11_SAMPLER_DESC materialSamplerDesc{};
    materialSamplerDesc.Filter = D3D11_FILTER_ANISOTROPIC;
    materialSamplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    materialSamplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    materialSamplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    materialSamplerDesc.MaxAnisotropy = 8;
    materialSamplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    ThrowIfFailed(device_->CreateSamplerState(&materialSamplerDesc, &materialSampler_),
                  "Cannot create the material sampler.");

    D3D11_SAMPLER_DESC environmentSamplerDesc = materialSamplerDesc;
    environmentSamplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    environmentSamplerDesc.MaxAnisotropy = 1;
    environmentSamplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    environmentSamplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    environmentSamplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ThrowIfFailed(device_->CreateSamplerState(&environmentSamplerDesc, &environmentSampler_),
                  "Cannot create the environment sampler.");

    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable = TRUE;
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    ThrowIfFailed(device_->CreateDepthStencilState(&depthDesc, &skyDepthState_),
                  "Cannot create the skybox depth state.");

    D3D11_RASTERIZER_DESC rasterizerDesc{};
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode = D3D11_CULL_NONE;
    rasterizerDesc.DepthClipEnable = TRUE;
    ThrowIfFailed(device_->CreateRasterizerState(&rasterizerDesc, &skyRasterizer_),
                  "Cannot create the skybox rasterizer state.");
}

void Application::CreateSkyboxGeometry()
{
    const SkyVertex vertices[] = {
        {{-1, -1, -1}}, {{-1,  1, -1}}, {{ 1,  1, -1}},
        {{-1, -1, -1}}, {{ 1,  1, -1}}, {{ 1, -1, -1}},
        {{ 1, -1,  1}}, {{ 1,  1,  1}}, {{-1,  1,  1}},
        {{ 1, -1,  1}}, {{-1,  1,  1}}, {{-1, -1,  1}},
        {{-1, -1,  1}}, {{-1,  1,  1}}, {{-1,  1, -1}},
        {{-1, -1,  1}}, {{-1,  1, -1}}, {{-1, -1, -1}},
        {{ 1, -1, -1}}, {{ 1,  1, -1}}, {{ 1,  1,  1}},
        {{ 1, -1, -1}}, {{ 1,  1,  1}}, {{ 1, -1,  1}},
        {{-1,  1, -1}}, {{-1,  1,  1}}, {{ 1,  1,  1}},
        {{-1,  1, -1}}, {{ 1,  1,  1}}, {{ 1,  1, -1}},
        {{-1, -1,  1}}, {{-1, -1, -1}}, {{ 1, -1, -1}},
        {{-1, -1,  1}}, {{ 1, -1, -1}}, {{ 1, -1,  1}}};

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(vertices);
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = vertices;
    ThrowIfFailed(device_->CreateBuffer(&desc, &data, &skyVertexBuffer_),
                  "Cannot create the skybox vertex buffer.");
}

void Application::LoadAssets()
{
    const std::filesystem::path assets = ExecutableDirectory() / L"assets";
    const std::filesystem::path cerberus = assets / L"Cerberus_by_Andrew_Maximov";
    model_.Load(device_.Get(), cerberus / L"Cerberus_LP.FBX");

    albedoMap_ = TextureLoader::LoadWICTexture(
        device_.Get(), context_.Get(), cerberus / L"Textures/Cerberus_A.jpg", true);
    normalMap_ = TextureLoader::LoadWICTexture(
        device_.Get(), context_.Get(), cerberus / L"Textures/Cerberus_N.jpg", false);
    metallicMap_ = TextureLoader::LoadWICTexture(
        device_.Get(), context_.Get(), cerberus / L"Textures/Cerberus_M.jpg", false);
    roughnessMap_ = TextureLoader::LoadWICTexture(
        device_.Get(), context_.Get(), cerberus / L"Textures/Cerberus_R.jpg", false);

    irradianceMap_ = TextureLoader::LoadDDS(device_.Get(), assets / L"IrradianceMap_BC6U.dds");
    prefilteredMap_ = TextureLoader::LoadDDS(device_.Get(), assets / L"PreFilteredEnvMap_BC6U.dds");
    brdfLut_ = TextureLoader::LoadDDS(device_.Get(), assets / L"IntegrationMap.dds");
}

void Application::Render(float deltaSeconds)
{
    if (autoRotate_)
    {
        modelAngle_ += deltaSeconds * 0.22f;
    }

    const float aspect = static_cast<float>(width_) / static_cast<float>(height_);
    const float cosPitch = std::cos(cameraPitch_);
    const XMVECTOR cameraPosition = XMVectorSet(
        cameraDistance_ * cosPitch * std::sin(cameraYaw_),
        cameraDistance_ * std::sin(cameraPitch_),
        -cameraDistance_ * cosPitch * std::cos(cameraYaw_),
        1.0f);

    const XMMATRIX world = model_.NormalizationTransform() * XMMatrixRotationY(modelAngle_);
    const XMMATRIX view = XMMatrixLookAtLH(
        cameraPosition, XMVectorZero(), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    const XMMATRIX projection = XMMatrixPerspectiveFovLH(
        XM_PIDIV4, aspect, 0.05f, 100.0f);

    SceneConstants constants{};
    XMStoreFloat4x4(&constants.world, world);
    XMStoreFloat4x4(&constants.view, view);
    XMStoreFloat4x4(&constants.projection, projection);
    XMStoreFloat4(&constants.cameraPosition, cameraPosition);
    constants.lightPosition[0] = XMFLOAT4(-3.6f,  3.0f, -2.8f, 1.0f);
    constants.lightPosition[1] = XMFLOAT4( 3.4f,  2.4f, -1.4f, 1.0f);
    constants.lightPosition[2] = XMFLOAT4(-2.8f, -1.2f,  3.0f, 1.0f);
    constants.lightPosition[3] = XMFLOAT4( 3.1f, -0.8f,  3.4f, 1.0f);
    constants.lightColor[0] = XMFLOAT4(85.0f, 67.0f, 50.0f, 1.0f);
    constants.lightColor[1] = XMFLOAT4(48.0f, 65.0f, 90.0f, 1.0f);
    constants.lightColor[2] = XMFLOAT4(70.0f, 42.0f, 30.0f, 1.0f);
    constants.lightColor[3] = XMFLOAT4(35.0f, 52.0f, 78.0f, 1.0f);
    constants.renderParams = XMFLOAT4(
        exposure_, iblEnabled_ ? 1.0f : 0.0f,
        directLightEnabled_ ? 1.0f : 0.0f, normalYSign_);
    constants.debugParams = XMFLOAT4(static_cast<float>(debugMode_), 0.0f, 0.0f, 0.0f);
    context_->UpdateSubresource(sceneConstantBuffer_.Get(), 0, nullptr, &constants, 0, 0);

    ID3D11RenderTargetView* renderTarget = renderTarget_.Get();
    context_->OMSetRenderTargets(1, &renderTarget, depthView_.Get());
    const float clearColor[] = {0.012f, 0.014f, 0.020f, 1.0f};
    context_->ClearRenderTargetView(renderTarget_.Get(), clearColor);
    context_->ClearDepthStencilView(depthView_.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                    1.0f, 0);

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width_);
    viewport.Height = static_cast<float>(height_);
    viewport.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &viewport);

    ID3D11Buffer* constantBuffer = sceneConstantBuffer_.Get();

    const UINT skyStride = sizeof(SkyVertex);
    const UINT skyOffset = 0;
    ID3D11Buffer* skyBuffer = skyVertexBuffer_.Get();
    context_->IASetInputLayout(skyInputLayout_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->IASetVertexBuffers(0, 1, &skyBuffer, &skyStride, &skyOffset);
    context_->VSSetShader(skyVertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(skyPixelShader_.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, &constantBuffer);
    context_->PSSetConstantBuffers(0, 1, &constantBuffer);
    ID3D11ShaderResourceView* skyResource = prefilteredMap_.Get();
    context_->PSSetShaderResources(0, 1, &skyResource);
    ID3D11SamplerState* environmentSampler = environmentSampler_.Get();
    context_->PSSetSamplers(0, 1, &environmentSampler);
    context_->OMSetDepthStencilState(skyDepthState_.Get(), 0);
    context_->RSSetState(skyRasterizer_.Get());
    context_->Draw(36, 0);

    ID3D11ShaderResourceView* nullResource = nullptr;
    context_->PSSetShaderResources(0, 1, &nullResource);
    context_->OMSetDepthStencilState(nullptr, 0);
    context_->RSSetState(nullptr);

    context_->IASetInputLayout(pbrInputLayout_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(pbrVertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(pbrPixelShader_.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, &constantBuffer);
    context_->PSSetConstantBuffers(0, 1, &constantBuffer);

    ID3D11ShaderResourceView* resources[] = {
        albedoMap_.Get(), normalMap_.Get(), metallicMap_.Get(), roughnessMap_.Get(),
        irradianceMap_.Get(), prefilteredMap_.Get(), brdfLut_.Get()};
    context_->PSSetShaderResources(0, 7, resources);
    ID3D11SamplerState* samplers[] = {materialSampler_.Get(), environmentSampler_.Get()};
    context_->PSSetSamplers(0, 2, samplers);
    model_.Draw(context_.Get());

    ThrowIfFailed(swapChain_->Present(1, 0), "Cannot present the rendered frame.");
}

void Application::UpdateWindowTitle()
{
    static const wchar_t* modeNames[] = {
        L"PBR", L"Albedo", L"World normal", L"Metallic", L"Roughness", L"N dot L"};
    std::wostringstream title;
    title.setf(std::ios::fixed);
    title.precision(2);
    title << L"PBR Lab | " << modeNames[debugMode_]
          << L" | IBL " << (iblEnabled_ ? L"ON" : L"OFF")
          << L" | Lights " << (directLightEnabled_ ? L"ON" : L"OFF")
          << L" | Exposure " << exposure_
          << L" | Normal Y " << (normalYSign_ < 0.0f ? L"flipped" : L"original");
    SetWindowTextW(window_, title.str().c_str());
}

int Application::Run()
{
    MSG message{};
    auto previousTime = std::chrono::steady_clock::now();

    while (message.message != WM_QUIT)
    {
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            continue;
        }

        const auto currentTime = std::chrono::steady_clock::now();
        const float deltaSeconds = std::chrono::duration<float>(currentTime - previousTime).count();
        previousTime = currentTime;
        if (width_ > 0 && height_ > 0)
        {
            Render(std::min(deltaSeconds, 0.1f));
        }
    }
    return static_cast<int>(message.wParam);
}

LRESULT Application::HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
        {
            Resize(LOWORD(lParam), HIWORD(lParam));
        }
        else
        {
            width_ = 0;
            height_ = 0;
        }
        return 0;

    case WM_LBUTTONDOWN:
        dragging_ = true;
        previousMouse_ = POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        SetCapture(window);
        return 0;

    case WM_LBUTTONUP:
        dragging_ = false;
        ReleaseCapture();
        return 0;

    case WM_MOUSEMOVE:
        if (dragging_)
        {
            const POINT current{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            cameraYaw_ += static_cast<float>(current.x - previousMouse_.x) * 0.008f;
            cameraPitch_ += static_cast<float>(current.y - previousMouse_.y) * 0.008f;
            cameraPitch_ = std::clamp(cameraPitch_, -1.35f, 1.35f);
            previousMouse_ = current;
        }
        return 0;

    case WM_MOUSEWHEEL:
    {
        const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) /
                            static_cast<float>(WHEEL_DELTA);
        cameraDistance_ *= std::pow(0.88f, steps);
        cameraDistance_ = std::clamp(cameraDistance_, 3.2f, 12.0f);
        return 0;
    }

    case WM_KEYDOWN:
        if ((lParam & (1LL << 30)) != 0)
        {
            return 0;
        }
        if (wParam >= VK_F1 && wParam <= VK_F6)
        {
            debugMode_ = static_cast<int>(wParam - VK_F1);
        }
        else if (wParam == 'I')
        {
            iblEnabled_ = !iblEnabled_;
        }
        else if (wParam == 'L')
        {
            directLightEnabled_ = !directLightEnabled_;
        }
        else if (wParam == 'Y')
        {
            normalYSign_ = -normalYSign_;
        }
        else if (wParam == VK_SPACE)
        {
            autoRotate_ = !autoRotate_;
        }
        else if (wParam == VK_OEM_PLUS || wParam == VK_ADD)
        {
            exposure_ = std::min(4.0f, exposure_ + 0.10f);
        }
        else if (wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT)
        {
            exposure_ = std::max(0.20f, exposure_ - 0.10f);
        }
        else if (wParam == VK_ESCAPE)
        {
            DestroyWindow(window);
            return 0;
        }
        UpdateWindowTitle();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try
    {
        Application application;
        application.Create(instance);
        const int result = application.Run();
        if (SUCCEEDED(comResult))
        {
            CoUninitialize();
        }
        return result;
    }
    catch (const std::exception& error)
    {
        MessageBoxA(nullptr, error.what(), "PBR Lab error", MB_OK | MB_ICONERROR);
        if (SUCCEEDED(comResult))
        {
            CoUninitialize();
        }
        return 1;
    }
}
