#include "SceneModel.h"

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
            throw std::runtime_error("Cannot determine executable directory.");
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
        ComPtr<ID3DBlob> result;
        ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3DCompileFromFile(
            path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entryPoint, target, flags, 0, &result, &errors);
        if (FAILED(hr))
        {
            std::string details;
            if (errors)
                details.assign(static_cast<const char*>(errors->GetBufferPointer()),
                               errors->GetBufferSize());
            throw std::runtime_error("Shader compilation failed: " + path.string() + "\n" + details);
        }
        return result;
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
    };

    struct alignas(16) PostConstants
    {
        XMFLOAT4 texelAndDirection;
        XMFLOAT4 bloomParams;
        XMFLOAT4 dofParams;
        XMFLOAT4 cameraParams;
    };

    struct RenderTexture
    {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11ShaderResourceView> srv;
    };

    static_assert(sizeof(SceneConstants) % 16 == 0);
    static_assert(sizeof(PostConstants) % 16 == 0);
}

class Application
{
public:
    void Create(HINSTANCE instance);
    int Run();
    LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

private:
    void CreateDeviceAndSwapChain();
    void CreatePipeline();
    void LoadScene();
    void Resize(UINT width, UINT height);
    void CreateSizeDependentResources(UINT width, UINT height);
    RenderTexture CreateRenderTexture(UINT width, UINT height, DXGI_FORMAT format);
    void Render(float deltaSeconds);
    void DrawFullScreen(ID3D11PixelShader* shader,
                        ID3D11RenderTargetView* target,
                        UINT width,
                        UINT height,
                        ID3D11ShaderResourceView* source0,
                        ID3D11ShaderResourceView* source1,
                        ID3D11ShaderResourceView* source2,
                        const PostConstants& constants);
    void UpdateWindowTitle();

    HWND window_ = nullptr;
    UINT width_ = 1280;
    UINT height_ = 720;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain> swapChain_;
    ComPtr<ID3D11RenderTargetView> backBufferView_;

    RenderTexture hdrScene_;
    RenderTexture blurA_;
    RenderTexture blurB_;
    ComPtr<ID3D11Texture2D> depthTexture_;
    ComPtr<ID3D11DepthStencilView> depthView_;
    ComPtr<ID3D11ShaderResourceView> depthSrv_;

    ComPtr<ID3D11VertexShader> sceneVS_;
    ComPtr<ID3D11PixelShader> scenePS_;
    ComPtr<ID3D11InputLayout> sceneLayout_;
    ComPtr<ID3D11VertexShader> fullScreenVS_;
    ComPtr<ID3D11PixelShader> brightPS_;
    ComPtr<ID3D11PixelShader> blurPS_;
    ComPtr<ID3D11PixelShader> finalPS_;
    ComPtr<ID3D11Buffer> sceneCB_;
    ComPtr<ID3D11Buffer> postCB_;
    ComPtr<ID3D11SamplerState> materialSampler_;
    ComPtr<ID3D11SamplerState> postSampler_;

    SceneModel scene_;

    bool dragging_ = false;
    POINT previousMouse_{};
    float cameraYaw_ = 0.12f;
    float cameraPitch_ = 0.10f;
    float cameraDistance_ = 6.0f;
    float sceneAngle_ = 0.0f;
    float exposure_ = 0.90f;
    float bloomThreshold_ = 1.05f;
    float bloomIntensity_ = 0.70f;
    float focusDistance_ = 5.8f;
    float focusRange_ = 1.25f;
    float maxDofRadius_ = 14.0f;
    float normalYSign_ = 1.0f;
    bool bloomEnabled_ = true;
    bool dofEnabled_ = true;
    bool autoRotate_ = true;
    int debugMode_ = 0;
};

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    Application* app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<Application*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    return app ? app->HandleMessage(window, message, wParam, lParam)
               : DefWindowProcW(window, message, wParam, lParam);
}

void Application::Create(HINSTANCE instance)
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProcedure;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"Lab07PostProcessingWindow";
    if (!RegisterClassExW(&wc))
        throw std::runtime_error("Cannot register the application window class.");

    RECT bounds{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE);
    window_ = CreateWindowExW(
        0, wc.lpszClassName, L"Lab 07: Post-Processing", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, instance, this);
    if (!window_)
        throw std::runtime_error("Cannot create the application window.");

    CreateDeviceAndSwapChain();
    CreatePipeline();
    LoadScene();
    UpdateWindowTitle();
    ShowWindow(window_, SW_SHOW);
}

void Application::CreateDeviceAndSwapChain()
{
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.Width = width_;
    desc.BufferDesc.Height = height_;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 1;
    desc.OutputWindow = window_;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selected{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
        D3D11_SDK_VERSION, &desc, &swapChain_, &device_, &selected, &context_);
    if (hr == E_INVALIDARG)
    {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &levels[1], 1,
            D3D11_SDK_VERSION, &desc, &swapChain_, &device_, &selected, &context_);
    }
    ThrowIfFailed(hr, "Cannot create a Direct3D 11 device.");
    CreateSizeDependentResources(width_, height_);
}

RenderTexture Application::CreateRenderTexture(UINT width, UINT height, DXGI_FORMAT format)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    RenderTexture result;
    ThrowIfFailed(device_->CreateTexture2D(&desc, nullptr, &result.texture),
                  "Cannot create post-processing texture.");
    ThrowIfFailed(device_->CreateRenderTargetView(result.texture.Get(), nullptr, &result.rtv),
                  "Cannot create post-processing render target.");
    ThrowIfFailed(device_->CreateShaderResourceView(result.texture.Get(), nullptr, &result.srv),
                  "Cannot create post-processing texture view.");
    return result;
}

void Application::CreateSizeDependentResources(UINT width, UINT height)
{
    ComPtr<ID3D11Texture2D> backBuffer;
    ThrowIfFailed(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)),
                  "Cannot get the swap-chain back buffer.");
    ThrowIfFailed(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &backBufferView_),
                  "Cannot create the back-buffer view.");

    hdrScene_ = CreateRenderTexture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    blurA_ = CreateRenderTexture(std::max(1u, width / 2u), std::max(1u, height / 2u),
                                 DXGI_FORMAT_R16G16B16A16_FLOAT);
    blurB_ = CreateRenderTexture(std::max(1u, width / 2u), std::max(1u, height / 2u),
                                 DXGI_FORMAT_R16G16B16A16_FLOAT);

    D3D11_TEXTURE2D_DESC depthDesc{};
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    ThrowIfFailed(device_->CreateTexture2D(&depthDesc, nullptr, &depthTexture_),
                  "Cannot create the readable depth texture.");

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ThrowIfFailed(device_->CreateDepthStencilView(depthTexture_.Get(), &dsvDesc, &depthView_),
                  "Cannot create the depth view.");

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    ThrowIfFailed(device_->CreateShaderResourceView(depthTexture_.Get(), &srvDesc, &depthSrv_),
                  "Cannot create the depth texture view.");
}

void Application::Resize(UINT width, UINT height)
{
    if (!swapChain_ || width == 0 || height == 0)
        return;

    ID3D11ShaderResourceView* nullSrvs[3] = {};
    context_->PSSetShaderResources(0, 3, nullSrvs);
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    backBufferView_.Reset();
    hdrScene_ = {};
    blurA_ = {};
    blurB_ = {};
    depthView_.Reset();
    depthSrv_.Reset();
    depthTexture_.Reset();

    width_ = width;
    height_ = height;
    ThrowIfFailed(swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0),
                  "Cannot resize the swap chain.");
    CreateSizeDependentResources(width, height);
}

void Application::CreatePipeline()
{
    const auto shaderDirectory = ExecutableDirectory() / L"shaders";
    const auto sceneVsBlob = CompileShader(shaderDirectory / L"Scene.hlsl", "VSMain", "vs_5_0");
    const auto scenePsBlob = CompileShader(shaderDirectory / L"Scene.hlsl", "PSMain", "ps_5_0");
    ThrowIfFailed(device_->CreateVertexShader(sceneVsBlob->GetBufferPointer(), sceneVsBlob->GetBufferSize(),
                                               nullptr, &sceneVS_), "Cannot create scene VS.");
    ThrowIfFailed(device_->CreatePixelShader(scenePsBlob->GetBufferPointer(), scenePsBlob->GetBufferSize(),
                                              nullptr, &scenePS_), "Cannot create scene PS.");

    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(SceneVertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(SceneVertex, normal), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(SceneVertex, texCoord), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(SceneVertex, tangent), D3D11_INPUT_PER_VERTEX_DATA, 0}};
    ThrowIfFailed(device_->CreateInputLayout(elements, static_cast<UINT>(std::size(elements)),
                                              sceneVsBlob->GetBufferPointer(), sceneVsBlob->GetBufferSize(),
                                              &sceneLayout_), "Cannot create scene input layout.");

    const auto fullScreenBlob = CompileShader(shaderDirectory / L"PostProcess.hlsl", "VSFullScreen", "vs_5_0");
    const auto brightBlob = CompileShader(shaderDirectory / L"PostProcess.hlsl", "PSBright", "ps_5_0");
    const auto blurBlob = CompileShader(shaderDirectory / L"PostProcess.hlsl", "PSBlur", "ps_5_0");
    const auto finalBlob = CompileShader(shaderDirectory / L"PostProcess.hlsl", "PSFinal", "ps_5_0");
    ThrowIfFailed(device_->CreateVertexShader(fullScreenBlob->GetBufferPointer(), fullScreenBlob->GetBufferSize(),
                                               nullptr, &fullScreenVS_), "Cannot create full-screen VS.");
    ThrowIfFailed(device_->CreatePixelShader(brightBlob->GetBufferPointer(), brightBlob->GetBufferSize(),
                                              nullptr, &brightPS_), "Cannot create bright-pass PS.");
    ThrowIfFailed(device_->CreatePixelShader(blurBlob->GetBufferPointer(), blurBlob->GetBufferSize(),
                                              nullptr, &blurPS_), "Cannot create blur PS.");
    ThrowIfFailed(device_->CreatePixelShader(finalBlob->GetBufferPointer(), finalBlob->GetBufferSize(),
                                              nullptr, &finalPS_), "Cannot create final PS.");

    D3D11_BUFFER_DESC sceneCbDesc{};
    sceneCbDesc.ByteWidth = sizeof(SceneConstants);
    sceneCbDesc.Usage = D3D11_USAGE_DEFAULT;
    sceneCbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ThrowIfFailed(device_->CreateBuffer(&sceneCbDesc, nullptr, &sceneCB_),
                  "Cannot create scene constant buffer.");
    D3D11_BUFFER_DESC postCbDesc = sceneCbDesc;
    postCbDesc.ByteWidth = sizeof(PostConstants);
    ThrowIfFailed(device_->CreateBuffer(&postCbDesc, nullptr, &postCB_),
                  "Cannot create post-processing constant buffer.");

    D3D11_SAMPLER_DESC materialDesc{};
    materialDesc.Filter = D3D11_FILTER_ANISOTROPIC;
    materialDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    materialDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    materialDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    materialDesc.MaxAnisotropy = 8;
    materialDesc.MaxLOD = D3D11_FLOAT32_MAX;
    ThrowIfFailed(device_->CreateSamplerState(&materialDesc, &materialSampler_),
                  "Cannot create material sampler.");

    D3D11_SAMPLER_DESC postDesc = materialDesc;
    postDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    postDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    postDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    postDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    postDesc.MaxAnisotropy = 1;
    ThrowIfFailed(device_->CreateSamplerState(&postDesc, &postSampler_),
                  "Cannot create post-processing sampler.");
}

void Application::LoadScene()
{
    const auto modelPath = ExecutableDirectory() / L"assets/Sponza-master/sponza.obj";
    scene_.Load(device_.Get(), context_.Get(), modelPath);
}

void Application::DrawFullScreen(ID3D11PixelShader* shader,
                                 ID3D11RenderTargetView* target,
                                 UINT width,
                                 UINT height,
                                 ID3D11ShaderResourceView* source0,
                                 ID3D11ShaderResourceView* source1,
                                 ID3D11ShaderResourceView* source2,
                                 const PostConstants& constants)
{
    ID3D11ShaderResourceView* nullSrvs[3] = {};
    context_->PSSetShaderResources(0, 3, nullSrvs);
    context_->OMSetRenderTargets(1, &target, nullptr);

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &viewport);

    context_->UpdateSubresource(postCB_.Get(), 0, nullptr, &constants, 0, 0);
    ID3D11Buffer* cb = postCB_.Get();
    ID3D11SamplerState* sampler = postSampler_.Get();
    ID3D11ShaderResourceView* resources[] = {source0, source1, source2};
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(fullScreenVS_.Get(), nullptr, 0);
    context_->PSSetShader(shader, nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, &cb);
    context_->PSSetSamplers(0, 1, &sampler);
    context_->PSSetShaderResources(0, 3, resources);
    context_->Draw(3, 0);
}

void Application::Render(float deltaSeconds)
{
    if (autoRotate_)
        sceneAngle_ += deltaSeconds * 0.075f;

    const float nearPlane = 0.05f;
    const float farPlane = 100.0f;
    const float aspect = static_cast<float>(width_) / height_;
    const float cosPitch = std::cos(cameraPitch_);
    const XMVECTOR cameraPosition = XMVectorSet(
        cameraDistance_ * cosPitch * std::sin(cameraYaw_),
        cameraDistance_ * std::sin(cameraPitch_),
        -cameraDistance_ * cosPitch * std::cos(cameraYaw_), 1.0f);
    const XMMATRIX world = scene_.NormalizationTransform() * XMMatrixRotationY(sceneAngle_);
    const XMMATRIX view = XMMatrixLookAtLH(cameraPosition, XMVectorZero(),
                                           XMVectorSet(0, 1, 0, 0));
    const XMMATRIX projection = XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, nearPlane, farPlane);

    SceneConstants sceneConstants{};
    XMStoreFloat4x4(&sceneConstants.world, world);
    XMStoreFloat4x4(&sceneConstants.view, view);
    XMStoreFloat4x4(&sceneConstants.projection, projection);
    XMStoreFloat4(&sceneConstants.cameraPosition, cameraPosition);
    sceneConstants.lightPosition[0] = XMFLOAT4(-1.6f,  1.4f, -1.2f, 1);
    sceneConstants.lightPosition[1] = XMFLOAT4( 1.4f,  1.1f, -0.2f, 1);
    sceneConstants.lightPosition[2] = XMFLOAT4(-0.7f, -0.2f,  1.5f, 1);
    sceneConstants.lightPosition[3] = XMFLOAT4( 1.3f,  0.2f,  1.4f, 1);
    sceneConstants.lightColor[0] = XMFLOAT4(14, 8, 4, 1);
    sceneConstants.lightColor[1] = XMFLOAT4(5, 9, 16, 1);
    sceneConstants.lightColor[2] = XMFLOAT4(12, 4, 3, 1);
    sceneConstants.lightColor[3] = XMFLOAT4(4, 7, 14, 1);
    sceneConstants.renderParams = XMFLOAT4(normalYSign_, 0, 0, 0);
    context_->UpdateSubresource(sceneCB_.Get(), 0, nullptr, &sceneConstants, 0, 0);

    ID3D11RenderTargetView* hdrTarget = hdrScene_.rtv.Get();
    context_->OMSetRenderTargets(1, &hdrTarget, depthView_.Get());
    const float clear[] = {0.003f, 0.004f, 0.007f, 1.0f};
    context_->ClearRenderTargetView(hdrTarget, clear);
    context_->ClearDepthStencilView(depthView_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    D3D11_VIEWPORT sceneViewport{};
    sceneViewport.Width = static_cast<float>(width_);
    sceneViewport.Height = static_cast<float>(height_);
    sceneViewport.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &sceneViewport);

    context_->IASetInputLayout(sceneLayout_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(sceneVS_.Get(), nullptr, 0);
    context_->PSSetShader(scenePS_.Get(), nullptr, 0);
    ID3D11Buffer* sceneCb = sceneCB_.Get();
    context_->VSSetConstantBuffers(0, 1, &sceneCb);
    context_->PSSetConstantBuffers(0, 1, &sceneCb);
    ID3D11SamplerState* materialSampler = materialSampler_.Get();
    context_->PSSetSamplers(0, 1, &materialSampler);
    scene_.Draw(context_.Get());

    context_->OMSetRenderTargets(0, nullptr, nullptr);
    const UINT halfWidth = std::max(1u, width_ / 2u);
    const UINT halfHeight = std::max(1u, height_ / 2u);

    PostConstants post{};
    post.texelAndDirection = XMFLOAT4(1.0f / halfWidth, 1.0f / halfHeight, 0, 0);
    post.bloomParams = XMFLOAT4(bloomThreshold_, 0.35f, bloomIntensity_, bloomEnabled_ ? 1.0f : 0.0f);
    post.dofParams = XMFLOAT4(focusDistance_, focusRange_, maxDofRadius_, dofEnabled_ ? 1.0f : 0.0f);
    post.cameraParams = XMFLOAT4(nearPlane, farPlane, exposure_, static_cast<float>(debugMode_));

    DrawFullScreen(brightPS_.Get(), blurA_.rtv.Get(), halfWidth, halfHeight,
                   hdrScene_.srv.Get(), nullptr, nullptr, post);

    ID3D11ShaderResourceView* currentBlur = blurA_.srv.Get();
    for (int pass = 0; pass < 10; ++pass)
    {
        const bool horizontal = (pass % 2) == 0;
        post.texelAndDirection = XMFLOAT4(
            1.0f / halfWidth, 1.0f / halfHeight,
            horizontal ? 1.0f : 0.0f, horizontal ? 0.0f : 1.0f);
        RenderTexture& target = horizontal ? blurB_ : blurA_;
        DrawFullScreen(blurPS_.Get(), target.rtv.Get(), halfWidth, halfHeight,
                       currentBlur, nullptr, nullptr, post);
        currentBlur = target.srv.Get();
    }

    post.texelAndDirection = XMFLOAT4(1.0f / width_, 1.0f / height_, 0, 0);
    DrawFullScreen(finalPS_.Get(), backBufferView_.Get(), width_, height_,
                   hdrScene_.srv.Get(), currentBlur, depthSrv_.Get(), post);

    ID3D11ShaderResourceView* nullSrvs[3] = {};
    context_->PSSetShaderResources(0, 3, nullSrvs);
    ThrowIfFailed(swapChain_->Present(1, 0), "Cannot present the rendered frame.");
}

void Application::UpdateWindowTitle()
{
    static const wchar_t* modes[] = {L"Composite", L"Original HDR", L"Bloom buffer", L"Depth", L"CoC"};
    std::wostringstream title;
    title.setf(std::ios::fixed);
    title.precision(2);
    title << L"Lab 07 | " << modes[debugMode_]
          << L" | Bloom " << (bloomEnabled_ ? L"ON" : L"OFF")
          << L" | DoF " << (dofEnabled_ ? L"ON" : L"OFF")
          << L" | Focus " << focusDistance_
          << L" | Exposure " << exposure_;
    SetWindowTextW(window_, title.str().c_str());
}

int Application::Run()
{
    MSG message{};
    auto previous = std::chrono::steady_clock::now();
    while (message.message != WM_QUIT)
    {
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            continue;
        }
        const auto current = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(current - previous).count();
        previous = current;
        if (width_ && height_)
            Render(std::min(dt, 0.1f));
    }
    return static_cast<int>(message.wParam);
}

LRESULT Application::HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
        {
            width_ = height_ = 0;
        }
        else
        {
            Resize(LOWORD(lParam), HIWORD(lParam));
        }
        return 0;
    case WM_LBUTTONDOWN:
        dragging_ = true;
        previousMouse_ = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        SetCapture(window);
        return 0;
    case WM_LBUTTONUP:
        dragging_ = false;
        ReleaseCapture();
        return 0;
    case WM_MOUSEMOVE:
        if (dragging_)
        {
            POINT current{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            cameraYaw_ += (current.x - previousMouse_.x) * 0.008f;
            cameraPitch_ = std::clamp(cameraPitch_ + (current.y - previousMouse_.y) * 0.008f,
                                      -1.30f, 1.30f);
            previousMouse_ = current;
        }
        return 0;
    case WM_MOUSEWHEEL:
    {
        const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
        cameraDistance_ = std::clamp(cameraDistance_ * std::pow(0.88f, steps), 3.0f, 12.0f);
        return 0;
    }
    case WM_KEYDOWN:
        if ((lParam & (1LL << 30)) != 0)
            return 0;
        if (wParam >= VK_F1 && wParam <= VK_F5)
            debugMode_ = static_cast<int>(wParam - VK_F1);
        else if (wParam == 'B') bloomEnabled_ = !bloomEnabled_;
        else if (wParam == 'D') dofEnabled_ = !dofEnabled_;
        else if (wParam == 'Y') normalYSign_ = -normalYSign_;
        else if (wParam == VK_SPACE) autoRotate_ = !autoRotate_;
        else if (wParam == 'Q') focusDistance_ = std::max(0.2f, focusDistance_ - 0.2f);
        else if (wParam == 'E') focusDistance_ = std::min(20.0f, focusDistance_ + 0.2f);
        else if (wParam == 'Z') focusRange_ = std::max(0.2f, focusRange_ - 0.1f);
        else if (wParam == 'X') focusRange_ = std::min(10.0f, focusRange_ + 0.1f);
        else if (wParam == VK_OEM_PLUS || wParam == VK_ADD) exposure_ = std::min(4.0f, exposure_ + 0.1f);
        else if (wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT) exposure_ = std::max(0.2f, exposure_ - 0.1f);
        else if (wParam == VK_ESCAPE) { DestroyWindow(window); return 0; }
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
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try
    {
        Application application;
        application.Create(instance);
        const int result = application.Run();
        if (SUCCEEDED(com)) CoUninitialize();
        return result;
    }
    catch (const std::exception& error)
    {
        MessageBoxA(nullptr, error.what(), "Lab 07 error", MB_OK | MB_ICONERROR);
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }
}

