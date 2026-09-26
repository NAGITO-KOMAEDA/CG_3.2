#include "GBuffer.hpp"

#include <stdexcept>

void GBuffer::Initialize(ID3D12Device* device, UINT width, UINT height)
{
    if (!device || width == 0 || height == 0)
    {
        throw std::invalid_argument("GBuffer::Initialize received invalid dimensions");
    }

    m_width = width;
    m_height = height;
    CreateDescriptorHeaps(device);
    CreateResources(device);
}

void GBuffer::Resize(ID3D12Device* device, UINT width, UINT height)
{
    if (width == 0 || height == 0 || (width == m_width && height == m_height))
    {
        return;
    }

    for (auto& resource : m_color)
    {
        resource.Reset();
    }
    m_depth.Reset();

    m_width = width;
    m_height = height;
    m_lightingState = false;
    CreateResources(device);
}

void GBuffer::CreateDescriptorHeaps(ID3D12Device* device)
{
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = RenderTargetCount;
    dx::Check(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap)), "Create GBuffer RTV heap");

    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
    dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvDesc.NumDescriptors = 1;
    dx::Check(device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&m_dsvHeap)), "Create GBuffer DSV heap");

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.NumDescriptors = RenderTargetCount + 1;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    dx::Check(device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&m_srvHeap)), "Create GBuffer SRV heap");

    m_rtvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_srvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void GBuffer::CreateResources(ID3D12Device* device)
{
    const auto defaultHeap = dx::HeapProperties(D3D12_HEAP_TYPE_DEFAULT);

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = m_width;
    textureDesc.Height = m_height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    auto rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    auto srv = m_srvHeap->GetCPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < RenderTargetCount; ++i)
    {
        textureDesc.Format = ColorFormats[i];

        D3D12_CLEAR_VALUE clear{};
        clear.Format = ColorFormats[i];
        clear.Color[0] = 0.0f;
        clear.Color[1] = 0.0f;
        clear.Color[2] = 0.0f;
        clear.Color[3] = 0.0f;

        dx::Check(
            device->CreateCommittedResource(
                &defaultHeap,
                D3D12_HEAP_FLAG_NONE,
                &textureDesc,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                &clear,
                IID_PPV_ARGS(&m_color[i])),
            "Create GBuffer color resource");

        device->CreateRenderTargetView(m_color[i].Get(), nullptr, rtv);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = ColorFormats[i];
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(m_color[i].Get(), &srvDesc, srv);

        rtv.ptr += m_rtvIncrement;
        srv.ptr += m_srvIncrement;
    }

    D3D12_RESOURCE_DESC depthDesc{};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = m_width;
    depthDesc.Height = m_height;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE depthClear{};
    depthClear.Format = DXGI_FORMAT_D32_FLOAT;
    depthClear.DepthStencil.Depth = 1.0f;

    dx::Check(
        device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &depthDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &depthClear,
            IID_PPV_ARGS(&m_depth)),
        "Create GBuffer depth resource");

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(m_depth.Get(), &dsvDesc, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv{};
    depthSrv.Format = DXGI_FORMAT_R32_FLOAT;
    depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depthSrv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_depth.Get(), &depthSrv, srv);
}

void GBuffer::TransitionToGeometry(ID3D12GraphicsCommandList* commandList)
{
    if (!m_lightingState)
    {
        return;
    }

    std::array<D3D12_RESOURCE_BARRIER, RenderTargetCount + 1> barriers{};
    for (UINT i = 0; i < RenderTargetCount; ++i)
    {
        barriers[i] = dx::TransitionBarrier(
            m_color[i].Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    barriers.back() = dx::TransitionBarrier(
        m_depth.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_DEPTH_WRITE);
    commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    m_lightingState = false;
}

void GBuffer::TransitionToLighting(ID3D12GraphicsCommandList* commandList)
{
    if (m_lightingState)
    {
        return;
    }

    std::array<D3D12_RESOURCE_BARRIER, RenderTargetCount + 1> barriers{};
    for (UINT i = 0; i < RenderTargetCount; ++i)
    {
        barriers[i] = dx::TransitionBarrier(
            m_color[i].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    barriers.back() = dx::TransitionBarrier(
        m_depth.Get(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    m_lightingState = true;
}

void GBuffer::ClearAndBind(ID3D12GraphicsCommandList* commandList) const
{
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, RenderTargetCount> rtvs{};
    auto current = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    constexpr float clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    for (UINT i = 0; i < RenderTargetCount; ++i)
    {
        rtvs[i] = current;
        commandList->ClearRenderTargetView(current, clearColor, 0, nullptr);
        current.ptr += m_rtvIncrement;
    }

    const auto dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    commandList->OMSetRenderTargets(RenderTargetCount, rtvs.data(), FALSE, &dsv);
}

