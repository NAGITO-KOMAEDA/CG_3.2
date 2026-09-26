#pragma once

#include "Camera.hpp"
#include "DxUtil.hpp"
#include "GBuffer.hpp"
#include "Scene.hpp"

#include <DirectXMath.h>
#include <dxgi1_6.h>
#include <windows.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <vector>

class RenderingSystem
{
public:
    RenderingSystem(HINSTANCE instance, int showCommand, bool smokeTest = false);
    ~RenderingSystem();

    int Run();

private:
    static constexpr UINT FrameCount = 2;
    static constexpr DXGI_FORMAT BackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    struct FrameConstants
    {
        DirectX::XMFLOAT4X4 viewProjection;
    };

    struct LightingConstants
    {
        DirectX::XMFLOAT4X4 inverseViewProjection;
        DirectX::XMFLOAT3 cameraPosition;
        float padding0;
        DirectX::XMFLOAT2 screenSize;
        DirectX::XMFLOAT2 inverseScreenSize;
    };

    struct LightConstants
    {
        DirectX::XMFLOAT4 colorIntensity;
        DirectX::XMFLOAT4 positionRange;
        DirectX::XMFLOAT4 directionInnerCos;
        DirectX::XMFLOAT4 parameters;
    };

    struct FrameResource
    {
        dx::ComPtr<ID3D12CommandAllocator> commandAllocator;
        dx::ComPtr<ID3D12Resource> constantBuffer;
        std::uint8_t* mappedConstants = nullptr;
        UINT64 fenceValue = 0;
    };

    void InitializeWindow();
    void InitializeD3D();
    void CreateBackBufferViews();
    void CreateFrameResources();
    void CreateRootSignatures();
    void CreatePipelineStates();
    void LoadScene();
    void CreateLights();

    void Update(float deltaSeconds);
    void Render();
    FrameResource& BeginFrame();
    void EndFrame(FrameResource& frame);
    void FlushGpu();
    void Resize(UINT width, UINT height);

    LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    HINSTANCE m_instance = nullptr;
    int m_showCommand = SW_SHOWDEFAULT;
    HWND m_window = nullptr;
    UINT m_width = 1600;
    UINT m_height = 900;
    bool m_minimized = false;
    bool m_initialized = false;
    bool m_smokeTest = false;

    std::array<bool, 256> m_keys{};
    bool m_mouseLook = false;
    POINT m_lastMouse{};

    dx::ComPtr<IDXGIFactory6> m_factory;
    dx::ComPtr<ID3D12Device> m_device;
    dx::ComPtr<ID3D12CommandQueue> m_commandQueue;
    dx::ComPtr<IDXGISwapChain3> m_swapChain;
    dx::ComPtr<ID3D12GraphicsCommandList> m_commandList;
    dx::ComPtr<ID3D12DescriptorHeap> m_backBufferRtvHeap;
    std::array<dx::ComPtr<ID3D12Resource>, FrameCount> m_backBuffers;
    std::array<FrameResource, FrameCount> m_frames;
    UINT m_backBufferRtvIncrement = 0;
    UINT m_frameIndex = 0;

    dx::ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    UINT64 m_nextFenceValue = 1;

    dx::ComPtr<ID3D12RootSignature> m_geometryRootSignature;
    dx::ComPtr<ID3D12RootSignature> m_lightingRootSignature;
    dx::ComPtr<ID3D12PipelineState> m_geometryPipeline;
    dx::ComPtr<ID3D12PipelineState> m_directionalPipeline;
    dx::ComPtr<ID3D12PipelineState> m_additiveLightPipeline;

    D3D12_VIEWPORT m_viewport{};
    D3D12_RECT m_scissor{};
    UINT m_frameConstantsOffset = 0;
    UINT m_lightingConstantsOffset = 0;
    UINT m_constantBufferSize = 0;

    Camera m_camera;
    GBuffer m_gBuffer;
    Scene m_scene;
    LightConstants m_directionalLight{};
    std::vector<LightConstants> m_pointLights;
    std::vector<LightConstants> m_spotLights;
    float m_elapsedSeconds = 0.0f;
};
