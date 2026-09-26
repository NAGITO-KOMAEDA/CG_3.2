#pragma once

#include "DxUtil.hpp"

#include <array>

class GBuffer
{
public:
    static constexpr UINT RenderTargetCount = 2;

    void Initialize(ID3D12Device* device, UINT width, UINT height);
    void Resize(ID3D12Device* device, UINT width, UINT height);

    void TransitionToGeometry(ID3D12GraphicsCommandList* commandList);
    void TransitionToLighting(ID3D12GraphicsCommandList* commandList);
    void ClearAndBind(ID3D12GraphicsCommandList* commandList) const;

    ID3D12DescriptorHeap* ShaderVisibleHeap() const { return m_srvHeap.Get(); }
    D3D12_GPU_DESCRIPTOR_HANDLE SrvTable() const { return m_srvHeap->GetGPUDescriptorHandleForHeapStart(); }
    DXGI_FORMAT ColorFormat(UINT index) const { return ColorFormats.at(index); }
    DXGI_FORMAT DepthFormat() const { return DXGI_FORMAT_D32_FLOAT; }

private:
    void CreateDescriptorHeaps(ID3D12Device* device);
    void CreateResources(ID3D12Device* device);

    static constexpr std::array<DXGI_FORMAT, RenderTargetCount> ColorFormats = {
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
    };

    UINT m_width = 0;
    UINT m_height = 0;
    UINT m_rtvIncrement = 0;
    UINT m_srvIncrement = 0;
    bool m_lightingState = false;

    std::array<dx::ComPtr<ID3D12Resource>, RenderTargetCount> m_color;
    dx::ComPtr<ID3D12Resource> m_depth;
    dx::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    dx::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    dx::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
};

