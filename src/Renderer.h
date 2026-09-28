#pragma once

#include "ObjLoader.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <vector>

class Renderer
{
public:
    Renderer() = default;
    ~Renderer() = default;

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void Initialize(HWND window, uint32_t width, uint32_t height, const std::filesystem::path& root);
    void Resize(uint32_t width, uint32_t height);
    void Update(float deltaSeconds);
    void Render();
    void ResetParticles();

    void SetPaused(bool paused) { paused_ = paused; }
    bool IsPaused() const { return paused_; }
    void MoveCamera(float forward, float right, float up, float deltaSeconds);
    void RotateCamera(float yawDelta, float pitchDelta);

    uint32_t ParticleCount() const { return kParticleCount; }
    const CpuMesh& SceneMesh() const { return sceneMesh_; }

private:
    struct SceneConstants
    {
        DirectX::XMFLOAT4X4 worldViewProjection{};
        DirectX::XMFLOAT4X4 world{};
        DirectX::XMFLOAT4 lightDirection{-0.45f, -0.8f, 0.3f, 0.0f};
        DirectX::XMFLOAT4 cameraPosition{};
    };

    struct MaterialConstants
    {
        DirectX::XMFLOAT4 diffuse{1.0f, 1.0f, 1.0f, 1.0f};
    };

    struct ParticleConstants
    {
        DirectX::XMFLOAT4X4 viewProjection{};
        DirectX::XMFLOAT4 cameraRight{};
        DirectX::XMFLOAT4 cameraUp{};
        DirectX::XMFLOAT4 emitterAndTime{};
        DirectX::XMFLOAT4 gravityAndDelta{};
        uint32_t particleCount = 0;
        uint32_t frameIndex = 0;
        DirectX::XMFLOAT2 padding{};
    };

    struct GpuMaterial
    {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> texture;
        DirectX::XMFLOAT4 diffuse{};
    };

    static constexpr uint32_t kParticleCount = 16384;

    void CreateDevice(HWND window);
    void CreateSizeDependentResources();
    void CreateStates();
    void CreateShaders(const std::filesystem::path& root);
    void LoadScene(const std::filesystem::path& root);
    void CreateParticleResources();
    void UpdateCameraConstants();
    void RenderScene();
    void RenderParticles();

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> LoadTga(const std::filesystem::path& path,
                                                             const DirectX::XMFLOAT4& fallbackColor);

    HWND window_ = nullptr;
    uint32_t width_ = 1;
    uint32_t height_ = 1;
    bool paused_ = false;
    uint32_t frameIndex_ = 0;
    uint32_t currentParticleBuffer_ = 0;
    float elapsedTime_ = 0.0f;

    DirectX::XMFLOAT3 cameraPosition_{0.0f, 5.0f, -15.0f};
    float cameraYaw_ = 0.0f;
    float cameraPitch_ = -0.05f;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTarget_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depthTexture_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depthView_;

    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerState_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> particleRasterizerState_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthState_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> opaqueBlendState_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerState_;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> sceneVertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> scenePixelShader_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> sceneInputLayout_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> particleVertexShader_;
    Microsoft::WRL::ComPtr<ID3D11GeometryShader> particleGeometryShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> particlePixelShader_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> particleComputeShader_;

    Microsoft::WRL::ComPtr<ID3D11Buffer> sceneConstantBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> materialConstantBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> particleConstantBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> sceneVertexBuffer_;

    std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, 2> particleBuffers_;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 2> particleSrvs_;
    std::array<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>, 2> particleUavs_;

    CpuMesh sceneMesh_;
    std::vector<GpuMaterial> gpuMaterials_;
    SceneConstants sceneConstants_{};
    ParticleConstants particleConstants_{};
};
