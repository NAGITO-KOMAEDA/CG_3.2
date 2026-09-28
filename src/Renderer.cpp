#include "Renderer.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace
{
void ThrowIfFailed(HRESULT result, const char* operation)
{
    if (FAILED(result))
        throw std::runtime_error(std::string(operation) + " failed (HRESULT " + std::to_string(static_cast<unsigned long>(result)) + ")");
}

ComPtr<ID3DBlob> CompileShader(const std::filesystem::path& path, const char* entryPoint, const char* profile)
{
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                              entryPoint, profile, flags, 0, &bytecode, &errors);
    if (FAILED(result))
    {
        const std::string message = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "Unknown shader compiler error";
        throw std::runtime_error(path.string() + ": " + message);
    }
    return bytecode;
}

template<typename T>
ComPtr<ID3D11Buffer> CreateConstantBuffer(ID3D11Device* device)
{
    static_assert((sizeof(T) % 16) == 0);
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = static_cast<UINT>(sizeof(T));
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> buffer;
    ThrowIfFailed(device->CreateBuffer(&description, nullptr, &buffer), "Create constant buffer");
    return buffer;
}

uint8_t ToByte(float value)
{
    return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

#pragma pack(push, 1)
struct TgaHeader
{
    uint8_t idLength;
    uint8_t colorMapType;
    uint8_t imageType;
    uint16_t colorMapFirst;
    uint16_t colorMapLength;
    uint8_t colorMapDepth;
    uint16_t xOrigin;
    uint16_t yOrigin;
    uint16_t width;
    uint16_t height;
    uint8_t bitsPerPixel;
    uint8_t descriptor;
};
#pragma pack(pop)
}

void Renderer::Initialize(HWND window, uint32_t width, uint32_t height, const std::filesystem::path& root)
{
    window_ = window;
    width_ = std::max(width, 1u);
    height_ = std::max(height, 1u);
    CreateDevice(window);
    CreateSizeDependentResources();
    CreateStates();
    CreateShaders(root);
    LoadScene(root);
    CreateParticleResources();
}

void Renderer::CreateDevice(HWND window)
{
    DXGI_SWAP_CHAIN_DESC swapDescription{};
    swapDescription.BufferDesc.Width = width_;
    swapDescription.BufferDesc.Height = height_;
    swapDescription.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDescription.SampleDesc.Count = 1;
    swapDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDescription.BufferCount = 2;
    swapDescription.OutputWindow = window;
    swapDescription.Windowed = TRUE;
    swapDescription.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const std::array featureLevels{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selectedFeatureLevel{};
    UINT flags = 0;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                                   featureLevels.data(), static_cast<UINT>(featureLevels.size()),
                                                   D3D11_SDK_VERSION, &swapDescription, &swapChain_, &device_,
                                                   &selectedFeatureLevel, &context_);
#if defined(_DEBUG)
    if (result == DXGI_ERROR_SDK_COMPONENT_MISSING)
    {
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                               featureLevels.data(), static_cast<UINT>(featureLevels.size()),
                                               D3D11_SDK_VERSION, &swapDescription, &swapChain_, &device_,
                                               &selectedFeatureLevel, &context_);
    }
#endif
    ThrowIfFailed(result, "D3D11CreateDeviceAndSwapChain");
    if (selectedFeatureLevel < D3D_FEATURE_LEVEL_11_0)
        throw std::runtime_error("Direct3D feature level 11.0 is required");
}

void Renderer::CreateSizeDependentResources()
{
    ComPtr<ID3D11Texture2D> backBuffer;
    ThrowIfFailed(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)), "Get swap-chain back buffer");
    ThrowIfFailed(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTarget_), "Create render target");

    D3D11_TEXTURE2D_DESC depthDescription{};
    depthDescription.Width = width_;
    depthDescription.Height = height_;
    depthDescription.MipLevels = 1;
    depthDescription.ArraySize = 1;
    depthDescription.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDescription.SampleDesc.Count = 1;
    depthDescription.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ThrowIfFailed(device_->CreateTexture2D(&depthDescription, nullptr, &depthTexture_), "Create depth texture");
    ThrowIfFailed(device_->CreateDepthStencilView(depthTexture_.Get(), nullptr, &depthView_), "Create depth view");
}

void Renderer::Resize(uint32_t width, uint32_t height)
{
    if (!swapChain_ || width == 0 || height == 0)
        return;
    width_ = width;
    height_ = height;
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    renderTarget_.Reset();
    depthView_.Reset();
    depthTexture_.Reset();
    ThrowIfFailed(swapChain_->ResizeBuffers(0, width_, height_, DXGI_FORMAT_UNKNOWN, 0), "Resize swap chain");
    CreateSizeDependentResources();
}

void Renderer::CreateStates()
{
    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    // The source OBJ uses mixed material-side conventions. Two-sided rasterization
    // keeps architectural cloth, chains and plant cards visible without alpha blending.
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.FrontCounterClockwise = FALSE;
    rasterizer.DepthClipEnable = TRUE;
    ThrowIfFailed(device_->CreateRasterizerState(&rasterizer, &rasterizerState_), "Create rasterizer state");
    rasterizer.CullMode = D3D11_CULL_NONE;
    ThrowIfFailed(device_->CreateRasterizerState(&rasterizer, &particleRasterizerState_), "Create particle rasterizer state");

    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    ThrowIfFailed(device_->CreateDepthStencilState(&depth, &depthState_), "Create depth state");

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = FALSE;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ThrowIfFailed(device_->CreateBlendState(&blend, &opaqueBlendState_), "Create opaque blend state");

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    ThrowIfFailed(device_->CreateSamplerState(&sampler, &samplerState_), "Create sampler state");
}

void Renderer::CreateShaders(const std::filesystem::path& root)
{
    const auto scenePath = root / L"shaders" / L"scene.hlsl";
    const auto particlePath = root / L"shaders" / L"particles.hlsl";

    const ComPtr<ID3DBlob> sceneVs = CompileShader(scenePath, "VSMain", "vs_5_0");
    const ComPtr<ID3DBlob> scenePs = CompileShader(scenePath, "PSMain", "ps_5_0");
    ThrowIfFailed(device_->CreateVertexShader(sceneVs->GetBufferPointer(), sceneVs->GetBufferSize(), nullptr, &sceneVertexShader_), "Create scene vertex shader");
    ThrowIfFailed(device_->CreatePixelShader(scenePs->GetBufferPointer(), scenePs->GetBufferSize(), nullptr, &scenePixelShader_), "Create scene pixel shader");

    const std::array<D3D11_INPUT_ELEMENT_DESC, 3> elements{{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, normal), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, texcoord), D3D11_INPUT_PER_VERTEX_DATA, 0},
    }};
    ThrowIfFailed(device_->CreateInputLayout(elements.data(), static_cast<UINT>(elements.size()),
                                              sceneVs->GetBufferPointer(), sceneVs->GetBufferSize(), &sceneInputLayout_),
                  "Create scene input layout");

    const ComPtr<ID3DBlob> particleVs = CompileShader(particlePath, "VSMain", "vs_5_0");
    const ComPtr<ID3DBlob> particleGs = CompileShader(particlePath, "GSMain", "gs_5_0");
    const ComPtr<ID3DBlob> particlePs = CompileShader(particlePath, "PSMain", "ps_5_0");
    const ComPtr<ID3DBlob> particleCs = CompileShader(particlePath, "CSMain", "cs_5_0");
    ThrowIfFailed(device_->CreateVertexShader(particleVs->GetBufferPointer(), particleVs->GetBufferSize(), nullptr, &particleVertexShader_), "Create particle vertex shader");
    ThrowIfFailed(device_->CreateGeometryShader(particleGs->GetBufferPointer(), particleGs->GetBufferSize(), nullptr, &particleGeometryShader_), "Create particle geometry shader");
    ThrowIfFailed(device_->CreatePixelShader(particlePs->GetBufferPointer(), particlePs->GetBufferSize(), nullptr, &particlePixelShader_), "Create particle pixel shader");
    ThrowIfFailed(device_->CreateComputeShader(particleCs->GetBufferPointer(), particleCs->GetBufferSize(), nullptr, &particleComputeShader_), "Create particle compute shader");

    sceneConstantBuffer_ = CreateConstantBuffer<SceneConstants>(device_.Get());
    materialConstantBuffer_ = CreateConstantBuffer<MaterialConstants>(device_.Get());
    particleConstantBuffer_ = CreateConstantBuffer<ParticleConstants>(device_.Get());
}

void Renderer::LoadScene(const std::filesystem::path& root)
{
    const auto scenePath = root / L"assets" / L"Sponza" / L"sponza.obj";
    sceneMesh_ = LoadObj(scenePath);

    D3D11_BUFFER_DESC vertexDescription{};
    vertexDescription.ByteWidth = static_cast<UINT>(sceneMesh_.vertices.size() * sizeof(Vertex));
    vertexDescription.Usage = D3D11_USAGE_DEFAULT;
    vertexDescription.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vertexData{};
    vertexData.pSysMem = sceneMesh_.vertices.data();
    ThrowIfFailed(device_->CreateBuffer(&vertexDescription, &vertexData, &sceneVertexBuffer_), "Create Sponza vertex buffer");

    gpuMaterials_.reserve(sceneMesh_.materials.size());
    for (const CpuMaterial& material : sceneMesh_.materials)
    {
        GpuMaterial gpu;
        gpu.diffuse = XMFLOAT4(material.diffuse.x, material.diffuse.y, material.diffuse.z, 1.0f);
        gpu.texture = LoadTga(material.diffuseTexture, gpu.diffuse);
        gpuMaterials_.push_back(std::move(gpu));
    }
}

ComPtr<ID3D11ShaderResourceView> Renderer::LoadTga(const std::filesystem::path& path, const XMFLOAT4& fallbackColor)
{
    uint16_t width = 1;
    uint16_t height = 1;
    std::vector<uint8_t> pixels{ToByte(fallbackColor.x), ToByte(fallbackColor.y), ToByte(fallbackColor.z), 255};

    std::ifstream file(path, std::ios::binary);
    if (file)
    {
        TgaHeader header{};
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (file && header.imageType == 2 && header.colorMapType == 0 &&
            (header.bitsPerPixel == 24 || header.bitsPerPixel == 32) && header.width > 0 && header.height > 0)
        {
            width = header.width;
            height = header.height;
            const uint32_t sourceStride = header.bitsPerPixel / 8;
            std::vector<uint8_t> source(static_cast<size_t>(width) * height * sourceStride);
            file.seekg(header.idLength, std::ios::cur);
            file.read(reinterpret_cast<char*>(source.data()), static_cast<std::streamsize>(source.size()));
            if (file)
            {
                pixels.resize(static_cast<size_t>(width) * height * 4);
                const bool topOrigin = (header.descriptor & 0x20u) != 0;
                for (uint32_t y = 0; y < height; ++y)
                {
                    const uint32_t sourceY = topOrigin ? y : (height - 1u - y);
                    for (uint32_t x = 0; x < width; ++x)
                    {
                        const size_t sourceOffset = (static_cast<size_t>(sourceY) * width + x) * sourceStride;
                        const size_t outputOffset = (static_cast<size_t>(y) * width + x) * 4;
                        pixels[outputOffset + 0] = source[sourceOffset + 2];
                        pixels[outputOffset + 1] = source[sourceOffset + 1];
                        pixels[outputOffset + 2] = source[sourceOffset + 0];
                        pixels[outputOffset + 3] = sourceStride == 4 ? source[sourceOffset + 3] : 255;
                    }
                }
            }
        }
    }

    D3D11_TEXTURE2D_DESC textureDescription{};
    textureDescription.Width = width;
    textureDescription.Height = height;
    textureDescription.MipLevels = 1;
    textureDescription.ArraySize = 1;
    textureDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    textureDescription.SampleDesc.Count = 1;
    textureDescription.Usage = D3D11_USAGE_IMMUTABLE;
    textureDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA textureData{};
    textureData.pSysMem = pixels.data();
    textureData.SysMemPitch = static_cast<UINT>(width) * 4;
    ComPtr<ID3D11Texture2D> texture;
    ThrowIfFailed(device_->CreateTexture2D(&textureDescription, &textureData, &texture), "Create material texture");
    ComPtr<ID3D11ShaderResourceView> view;
    ThrowIfFailed(device_->CreateShaderResourceView(texture.Get(), nullptr, &view), "Create material texture view");
    return view;
}

void Renderer::CreateParticleResources()
{
    std::vector<Particle> particles(kParticleCount);
    std::mt19937 random(0xC0FFEEu);
    std::uniform_real_distribution<float> angleDistribution(0.0f, XM_2PI);
    std::uniform_real_distribution<float> radiusDistribution(0.0f, 1.0f);
    std::uniform_real_distribution<float> speedDistribution(2.5f, 5.5f);
    std::uniform_real_distribution<float> lifetimeDistribution(2.0f, 5.0f);
    std::uniform_real_distribution<float> ageDistribution(0.0f, 1.0f);

    for (Particle& particle : particles)
    {
        const float angle = angleDistribution(random);
        const float radius = std::sqrt(radiusDistribution(random)) * 0.35f;
        const float speed = speedDistribution(random);
        particle.position = XMFLOAT3(std::cos(angle) * radius, 0.15f, std::sin(angle) * radius);
        particle.velocity = XMFLOAT3(std::cos(angle) * 0.45f, speed, std::sin(angle) * 0.45f);
        particle.lifetime = lifetimeDistribution(random);
        particle.age = ageDistribution(random) * particle.lifetime;
        particle.color = XMFLOAT4(1.0f, 0.35f + radiusDistribution(random) * 0.5f, 0.08f, 1.0f);
        particle.size = 0.055f + radiusDistribution(random) * 0.045f;
    }

    for (uint32_t i = 0; i < 2; ++i)
    {
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = sizeof(Particle) * kParticleCount;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        description.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        description.StructureByteStride = sizeof(Particle);
        D3D11_SUBRESOURCE_DATA initialData{};
        initialData.pSysMem = particles.data();
        ThrowIfFailed(device_->CreateBuffer(&description, i == 0 ? &initialData : nullptr, &particleBuffers_[i]), "Create particle buffer");

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDescription{};
        srvDescription.Format = DXGI_FORMAT_UNKNOWN;
        srvDescription.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDescription.Buffer.NumElements = kParticleCount;
        ThrowIfFailed(device_->CreateShaderResourceView(particleBuffers_[i].Get(), &srvDescription, &particleSrvs_[i]), "Create particle SRV");

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDescription{};
        uavDescription.Format = DXGI_FORMAT_UNKNOWN;
        uavDescription.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDescription.Buffer.NumElements = kParticleCount;
        uavDescription.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_APPEND;
        ThrowIfFailed(device_->CreateUnorderedAccessView(particleBuffers_[i].Get(), &uavDescription, &particleUavs_[i]), "Create particle UAV");
    }
    currentParticleBuffer_ = 0;
    frameIndex_ = 0;

    ID3D11UnorderedAccessView* initializeUav = particleUavs_[0].Get();
    const UINT initialCount = kParticleCount;
    context_->CSSetUnorderedAccessViews(0, 1, &initializeUav, &initialCount);
    ID3D11UnorderedAccessView* nullUav = nullptr;
    context_->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
}

void Renderer::ResetParticles()
{
    for (auto& buffer : particleBuffers_) buffer.Reset();
    for (auto& view : particleSrvs_) view.Reset();
    for (auto& view : particleUavs_) view.Reset();
    CreateParticleResources();
}

void Renderer::MoveCamera(float forward, float right, float up, float deltaSeconds)
{
    const float speed = 5.0f * deltaSeconds;
    const XMVECTOR forwardVector = XMVector3Normalize(XMVectorSet(std::sin(cameraYaw_), 0.0f, std::cos(cameraYaw_), 0.0f));
    const XMVECTOR rightVector = XMVector3Normalize(XMVector3Cross(XMVectorSet(0, 1, 0, 0), forwardVector));
    XMVECTOR position = XMLoadFloat3(&cameraPosition_);
    position += forwardVector * (forward * speed);
    position += rightVector * (right * speed);
    position += XMVectorSet(0, 1, 0, 0) * (up * speed);
    XMStoreFloat3(&cameraPosition_, position);
}

void Renderer::RotateCamera(float yawDelta, float pitchDelta)
{
    cameraYaw_ += yawDelta;
    cameraPitch_ = std::clamp(cameraPitch_ + pitchDelta, -1.35f, 1.35f);
}

void Renderer::UpdateCameraConstants()
{
    const XMVECTOR eye = XMLoadFloat3(&cameraPosition_);
    const float cp = std::cos(cameraPitch_);
    const XMVECTOR forward = XMVector3Normalize(XMVectorSet(std::sin(cameraYaw_) * cp, std::sin(cameraPitch_), std::cos(cameraYaw_) * cp, 0.0f));
    const XMVECTOR right = XMVector3Normalize(XMVector3Cross(XMVectorSet(0, 1, 0, 0), forward));
    const XMVECTOR up = XMVector3Normalize(XMVector3Cross(forward, right));
    const XMMATRIX view = XMMatrixLookToLH(eye, forward, up);
    const XMMATRIX projection = XMMatrixPerspectiveFovLH(XMConvertToRadians(65.0f), static_cast<float>(width_) / height_, 0.05f, 250.0f);
    const XMMATRIX world = XMMatrixScaling(0.01f, 0.01f, 0.01f);

    XMStoreFloat4x4(&sceneConstants_.worldViewProjection, XMMatrixTranspose(world * view * projection));
    XMStoreFloat4x4(&sceneConstants_.world, XMMatrixTranspose(world));
    sceneConstants_.cameraPosition = XMFLOAT4(cameraPosition_.x, cameraPosition_.y, cameraPosition_.z, 1.0f);

    XMStoreFloat4x4(&particleConstants_.viewProjection, XMMatrixTranspose(view * projection));
    XMStoreFloat4(&particleConstants_.cameraRight, right);
    XMStoreFloat4(&particleConstants_.cameraUp, up);
}

void Renderer::Update(float deltaSeconds)
{
    deltaSeconds = std::min(deltaSeconds, 1.0f / 20.0f);
    const float simulationDelta = paused_ ? 0.0f : deltaSeconds;
    elapsedTime_ += simulationDelta;
    UpdateCameraConstants();

    particleConstants_.emitterAndTime = XMFLOAT4(0.0f, 0.15f, 0.0f, elapsedTime_);
    particleConstants_.gravityAndDelta = XMFLOAT4(0.0f, -3.6f, 0.0f, simulationDelta);
    particleConstants_.particleCount = kParticleCount;
    particleConstants_.frameIndex = frameIndex_++;
    context_->UpdateSubresource(particleConstantBuffer_.Get(), 0, nullptr, &particleConstants_, 0, 0);

    if (paused_)
        return;

    const uint32_t next = 1u - currentParticleBuffer_;
    ID3D11ShaderResourceView* nullSrv = nullptr;
    context_->VSSetShaderResources(0, 1, &nullSrv);
    context_->CSSetShader(particleComputeShader_.Get(), nullptr, 0);
    ID3D11Buffer* constantBuffer = particleConstantBuffer_.Get();
    context_->CSSetConstantBuffers(0, 1, &constantBuffer);
    std::array<ID3D11UnorderedAccessView*, 2> uavs{particleUavs_[currentParticleBuffer_].Get(), particleUavs_[next].Get()};
    std::array<UINT, 2> counts{static_cast<UINT>(-1), 0u};
    context_->CSSetUnorderedAccessViews(0, static_cast<UINT>(uavs.size()), uavs.data(), counts.data());
    context_->Dispatch((kParticleCount + 255u) / 256u, 1, 1);

    const std::array<ID3D11UnorderedAccessView*, 2> nullUavs{nullptr, nullptr};
    context_->CSSetUnorderedAccessViews(0, static_cast<UINT>(nullUavs.size()), nullUavs.data(), nullptr);
    context_->CSSetShader(nullptr, nullptr, 0);
    currentParticleBuffer_ = next;
}

void Renderer::Render()
{
    const std::array<float, 4> clearColor{0.025f, 0.035f, 0.055f, 1.0f};
    context_->ClearRenderTargetView(renderTarget_.Get(), clearColor.data());
    context_->ClearDepthStencilView(depthView_.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    ID3D11RenderTargetView* renderTarget = renderTarget_.Get();
    context_->OMSetRenderTargets(1, &renderTarget, depthView_.Get());
    const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
    context_->RSSetViewports(1, &viewport);
    context_->RSSetState(rasterizerState_.Get());
    context_->OMSetDepthStencilState(depthState_.Get(), 0);
    context_->OMSetBlendState(opaqueBlendState_.Get(), nullptr, 0xffffffffu);

    RenderScene();
    RenderParticles();
    ThrowIfFailed(swapChain_->Present(1, 0), "Present");
}

void Renderer::RenderScene()
{
    context_->UpdateSubresource(sceneConstantBuffer_.Get(), 0, nullptr, &sceneConstants_, 0, 0);
    const UINT stride = sizeof(Vertex);
    const UINT offset = 0;
    ID3D11Buffer* vertexBuffer = sceneVertexBuffer_.Get();
    context_->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
    context_->IASetInputLayout(sceneInputLayout_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(sceneVertexShader_.Get(), nullptr, 0);
    ID3D11Buffer* sceneBuffer = sceneConstantBuffer_.Get();
    context_->VSSetConstantBuffers(0, 1, &sceneBuffer);
    context_->GSSetShader(nullptr, nullptr, 0);
    context_->PSSetShader(scenePixelShader_.Get(), nullptr, 0);
    ID3D11SamplerState* sampler = samplerState_.Get();
    context_->PSSetSamplers(0, 1, &sampler);

    for (const MeshPart& part : sceneMesh_.parts)
    {
        const GpuMaterial& material = gpuMaterials_[part.materialIndex];
        const MaterialConstants constants{material.diffuse};
        context_->UpdateSubresource(materialConstantBuffer_.Get(), 0, nullptr, &constants, 0, 0);
        ID3D11Buffer* materialBuffer = materialConstantBuffer_.Get();
        context_->PSSetConstantBuffers(1, 1, &materialBuffer);
        ID3D11ShaderResourceView* texture = material.texture.Get();
        context_->PSSetShaderResources(0, 1, &texture);
        context_->Draw(part.vertexCount, part.firstVertex);
    }
    ID3D11ShaderResourceView* nullTexture = nullptr;
    context_->PSSetShaderResources(0, 1, &nullTexture);
}

void Renderer::RenderParticles()
{
    context_->RSSetState(particleRasterizerState_.Get());
    context_->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    context_->VSSetShader(particleVertexShader_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* particles = particleSrvs_[currentParticleBuffer_].Get();
    context_->VSSetShaderResources(0, 1, &particles);
    context_->GSSetShader(particleGeometryShader_.Get(), nullptr, 0);
    ID3D11Buffer* constants = particleConstantBuffer_.Get();
    context_->GSSetConstantBuffers(0, 1, &constants);
    context_->PSSetShader(particlePixelShader_.Get(), nullptr, 0);
    context_->Draw(kParticleCount, 0);

    ID3D11ShaderResourceView* nullSrv = nullptr;
    context_->VSSetShaderResources(0, 1, &nullSrv);
    context_->GSSetShader(nullptr, nullptr, 0);
}
