#include "RenderingSystem.hpp"

#include <d3d12sdklayers.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>

using namespace DirectX;

namespace
{
D3D12_RASTERIZER_DESC DefaultRasterizer()
{
    D3D12_RASTERIZER_DESC desc{};
    desc.FillMode = D3D12_FILL_MODE_SOLID;
    desc.CullMode = D3D12_CULL_MODE_NONE;
    desc.FrontCounterClockwise = FALSE;
    desc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    desc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    desc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    desc.DepthClipEnable = TRUE;
    desc.MultisampleEnable = FALSE;
    desc.AntialiasedLineEnable = FALSE;
    desc.ForcedSampleCount = 0;
    desc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    return desc;
}

D3D12_BLEND_DESC DefaultBlend()
{
    D3D12_BLEND_DESC desc{};
    desc.AlphaToCoverageEnable = FALSE;
    desc.IndependentBlendEnable = FALSE;
    auto& target = desc.RenderTarget[0];
    target.BlendEnable = FALSE;
    target.LogicOpEnable = FALSE;
    target.SrcBlend = D3D12_BLEND_ONE;
    target.DestBlend = D3D12_BLEND_ZERO;
    target.BlendOp = D3D12_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D12_BLEND_ONE;
    target.DestBlendAlpha = D3D12_BLEND_ZERO;
    target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    target.LogicOp = D3D12_LOGIC_OP_NOOP;
    target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    return desc;
}

D3D12_DEPTH_STENCIL_DESC DefaultDepthStencil()
{
    D3D12_DEPTH_STENCIL_DESC desc{};
    desc.DepthEnable = TRUE;
    desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    desc.StencilEnable = FALSE;
    desc.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    desc.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    return desc;
}

D3D12_SHADER_BYTECODE Bytecode(ID3DBlob* blob)
{
    return {blob->GetBufferPointer(), blob->GetBufferSize()};
}
}

RenderingSystem::RenderingSystem(HINSTANCE instance, int showCommand, bool smokeTest)
    : m_instance(instance), m_showCommand(showCommand), m_smokeTest(smokeTest)
{
}

RenderingSystem::~RenderingSystem()
{
    if (m_commandQueue && m_fence)
    {
        try
        {
            FlushGpu();
        }
        catch (...)
        {
        }
    }

    for (auto& frame : m_frames)
    {
        if (frame.constantBuffer && frame.mappedConstants)
        {
            frame.constantBuffer->Unmap(0, nullptr);
            frame.mappedConstants = nullptr;
        }
    }

    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
}

int RenderingSystem::Run()
{
    InitializeWindow();
    InitializeD3D();
    ShowWindow(m_window, m_smokeTest ? SW_HIDE : m_showCommand);
    UpdateWindow(m_window);

    MSG message{};
    auto previousTime = std::chrono::steady_clock::now();

    UINT renderedFrames = 0;
    while (message.message != WM_QUIT)
    {
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            continue;
        }

        const auto currentTime = std::chrono::steady_clock::now();
        const float deltaSeconds = std::min(
            std::chrono::duration<float>(currentTime - previousTime).count(),
            0.1f);
        previousTime = currentTime;

        if (!m_minimized)
        {
            Update(deltaSeconds);
            Render();
            if (m_smokeTest && ++renderedFrames >= 3)
            {
                DestroyWindow(m_window);
            }
        }
        else
        {
            WaitMessage();
        }
    }

    FlushGpu();
    return static_cast<int>(message.wParam);
}

void RenderingSystem::InitializeWindow()
{
    const wchar_t* className = L"DeferredSponzaWindow";
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = m_instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = className;

    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        throw std::runtime_error("RegisterClassExW failed");
    }

    RECT clientRect{0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height)};
    AdjustWindowRect(&clientRect, WS_OVERLAPPEDWINDOW, FALSE);

    m_window = CreateWindowExW(
        0,
        className,
        L"Deferred Sponza - DirectX 12",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        clientRect.right - clientRect.left,
        clientRect.bottom - clientRect.top,
        nullptr,
        nullptr,
        m_instance,
        this);

    if (!m_window)
    {
        throw std::runtime_error("CreateWindowExW failed");
    }
}

void RenderingSystem::InitializeD3D()
{
    UINT factoryFlags = 0;
#if defined(_DEBUG)
    dx::ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
    {
        debugController->EnableDebugLayer();
        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif

    dx::Check(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory)), "CreateDXGIFactory2");

    dx::ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0;
         m_factory->EnumAdapterByGpuPreference(
             index,
             DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
             IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         ++index)
    {
        DXGI_ADAPTER_DESC1 description{};
        adapter->GetDesc1(&description);
        if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
            SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), nullptr)))
        {
            break;
        }
        adapter.Reset();
    }

    if (!adapter)
    {
        dx::Check(m_factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "EnumWarpAdapter");
    }
    dx::Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device)), "D3D12CreateDevice");

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    dx::Check(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)), "Create command queue");

    DXGI_SWAP_CHAIN_DESC1 swapDesc{};
    swapDesc.Width = m_width;
    swapDesc.Height = m_height;
    swapDesc.Format = BackBufferFormat;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = FrameCount;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapDesc.Scaling = DXGI_SCALING_STRETCH;
    swapDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    dx::ComPtr<IDXGISwapChain1> baseSwapChain;
    dx::Check(
        m_factory->CreateSwapChainForHwnd(
            m_commandQueue.Get(),
            m_window,
            &swapDesc,
            nullptr,
            nullptr,
            &baseSwapChain),
        "Create swap chain");
    dx::Check(baseSwapChain.As(&m_swapChain), "Query IDXGISwapChain3");
    m_factory->MakeWindowAssociation(m_window, DXGI_MWA_NO_ALT_ENTER);
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = FrameCount;
    dx::Check(
        m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_backBufferRtvHeap)),
        "Create back buffer RTV heap");
    m_backBufferRtvIncrement = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    CreateBackBufferViews();

    dx::Check(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)), "Create fence");
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent)
    {
        throw std::runtime_error("CreateEventW failed");
    }

    CreateFrameResources();
    dx::Check(
        m_device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_frames[0].commandAllocator.Get(),
            nullptr,
            IID_PPV_ARGS(&m_commandList)),
        "Create command list");

    m_gBuffer.Initialize(m_device.Get(), m_width, m_height);
    CreateRootSignatures();
    CreatePipelineStates();
    CreateLights();
    LoadScene();

    dx::Check(m_commandList->Close(), "Close initialization command list");
    ID3D12CommandList* lists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, lists);
    FlushGpu();
    m_scene.ReleaseUploadResources();

    m_viewport = {0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f};
    m_scissor = {0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height)};
    m_camera.SetAspect(static_cast<float>(m_width) / static_cast<float>(m_height));
    m_initialized = true;
}

void RenderingSystem::CreateBackBufferViews()
{
    auto handle = m_backBufferRtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < FrameCount; ++i)
    {
        dx::Check(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])), "Get swap chain buffer");
        m_device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, handle);
        handle.ptr += m_backBufferRtvIncrement;
    }
}

void RenderingSystem::CreateFrameResources()
{
    m_frameConstantsOffset = 0;
    m_lightingConstantsOffset = dx::AlignConstantBuffer(sizeof(FrameConstants));
    m_constantBufferSize = m_lightingConstantsOffset + dx::AlignConstantBuffer(sizeof(LightingConstants));

    const auto uploadHeap = dx::HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = m_constantBufferSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for (auto& frame : m_frames)
    {
        dx::Check(
            m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.commandAllocator)),
            "Create frame command allocator");
        dx::Check(
            m_device->CreateCommittedResource(
                &uploadHeap,
                D3D12_HEAP_FLAG_NONE,
                &bufferDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&frame.constantBuffer)),
            "Create frame constant buffer");
        const D3D12_RANGE readRange{0, 0};
        dx::Check(
            frame.constantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&frame.mappedConstants)),
            "Map frame constant buffer");
    }
}

void RenderingSystem::CreateRootSignatures()
{
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MipLODBias = 0.0f;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_DESCRIPTOR_RANGE geometryRange{};
    geometryRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    geometryRange.NumDescriptors = 1;
    geometryRange.BaseShaderRegister = 0;
    geometryRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    std::array<D3D12_ROOT_PARAMETER, 3> geometryParameters{};
    geometryParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    geometryParameters[0].Descriptor.ShaderRegister = 0;
    geometryParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    geometryParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    geometryParameters[1].DescriptorTable.NumDescriptorRanges = 1;
    geometryParameters[1].DescriptorTable.pDescriptorRanges = &geometryRange;
    geometryParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    geometryParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    geometryParameters[2].Constants.Num32BitValues = 8;
    geometryParameters[2].Constants.ShaderRegister = 1;
    geometryParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC geometryDesc{};
    geometryDesc.NumParameters = static_cast<UINT>(geometryParameters.size());
    geometryDesc.pParameters = geometryParameters.data();
    geometryDesc.NumStaticSamplers = 1;
    geometryDesc.pStaticSamplers = &sampler;
    geometryDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    dx::ComPtr<ID3DBlob> signature;
    dx::ComPtr<ID3DBlob> errors;
    HRESULT hr = D3D12SerializeRootSignature(
        &geometryDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &signature,
        &errors);
    if (errors) OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
    dx::Check(hr, "Serialize geometry root signature");
    dx::Check(
        m_device->CreateRootSignature(
            0,
            signature->GetBufferPointer(),
            signature->GetBufferSize(),
            IID_PPV_ARGS(&m_geometryRootSignature)),
        "Create geometry root signature");

    D3D12_DESCRIPTOR_RANGE lightingRange{};
    lightingRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    lightingRange.NumDescriptors = GBuffer::RenderTargetCount + 1;
    lightingRange.BaseShaderRegister = 0;
    lightingRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    std::array<D3D12_ROOT_PARAMETER, 3> lightingParameters{};
    lightingParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    lightingParameters[0].DescriptorTable.NumDescriptorRanges = 1;
    lightingParameters[0].DescriptorTable.pDescriptorRanges = &lightingRange;
    lightingParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    lightingParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    lightingParameters[1].Descriptor.ShaderRegister = 0;
    lightingParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    lightingParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    lightingParameters[2].Constants.Num32BitValues = 16;
    lightingParameters[2].Constants.ShaderRegister = 1;
    lightingParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC lightingDesc{};
    lightingDesc.NumParameters = static_cast<UINT>(lightingParameters.size());
    lightingDesc.pParameters = lightingParameters.data();
    lightingDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    signature.Reset();
    errors.Reset();
    hr = D3D12SerializeRootSignature(
        &lightingDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &signature,
        &errors);
    if (errors) OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
    dx::Check(hr, "Serialize lighting root signature");
    dx::Check(
        m_device->CreateRootSignature(
            0,
            signature->GetBufferPointer(),
            signature->GetBufferSize(),
            IID_PPV_ARGS(&m_lightingRootSignature)),
        "Create lighting root signature");
}

void RenderingSystem::CreatePipelineStates()
{
    const auto shaderDirectory = dx::ExecutableDirectory() / "shaders";
    const auto geometryVs = dx::CompileShader(shaderDirectory / "GBuffer.hlsl", "VSMain", "vs_5_1");
    const auto geometryPs = dx::CompileShader(shaderDirectory / "GBuffer.hlsl", "PSMain", "ps_5_1");
    const auto lightingVs = dx::CompileShader(shaderDirectory / "Lighting.hlsl", "VSMain", "vs_5_1");
    const auto lightingPs = dx::CompileShader(shaderDirectory / "Lighting.hlsl", "PSMain", "ps_5_1");

    const std::array<D3D12_INPUT_ELEMENT_DESC, 3> inputLayout = {{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    }};

    D3D12_GRAPHICS_PIPELINE_STATE_DESC geometryDesc{};
    geometryDesc.pRootSignature = m_geometryRootSignature.Get();
    geometryDesc.VS = Bytecode(geometryVs.Get());
    geometryDesc.PS = Bytecode(geometryPs.Get());
    geometryDesc.BlendState = DefaultBlend();
    geometryDesc.SampleMask = UINT_MAX;
    geometryDesc.RasterizerState = DefaultRasterizer();
    geometryDesc.DepthStencilState = DefaultDepthStencil();
    geometryDesc.InputLayout = {inputLayout.data(), static_cast<UINT>(inputLayout.size())};
    geometryDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    geometryDesc.NumRenderTargets = GBuffer::RenderTargetCount;
    for (UINT i = 0; i < GBuffer::RenderTargetCount; ++i)
    {
        geometryDesc.RTVFormats[i] = m_gBuffer.ColorFormat(i);
    }
    geometryDesc.DSVFormat = m_gBuffer.DepthFormat();
    geometryDesc.SampleDesc.Count = 1;
    dx::Check(
        m_device->CreateGraphicsPipelineState(&geometryDesc, IID_PPV_ARGS(&m_geometryPipeline)),
        "Create geometry pipeline state");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC lightingDesc{};
    lightingDesc.pRootSignature = m_lightingRootSignature.Get();
    lightingDesc.VS = Bytecode(lightingVs.Get());
    lightingDesc.PS = Bytecode(lightingPs.Get());
    lightingDesc.BlendState = DefaultBlend();
    lightingDesc.SampleMask = UINT_MAX;
    lightingDesc.RasterizerState = DefaultRasterizer();
    lightingDesc.DepthStencilState = DefaultDepthStencil();
    lightingDesc.DepthStencilState.DepthEnable = FALSE;
    lightingDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    lightingDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    lightingDesc.NumRenderTargets = 1;
    lightingDesc.RTVFormats[0] = BackBufferFormat;
    lightingDesc.SampleDesc.Count = 1;
    dx::Check(
        m_device->CreateGraphicsPipelineState(&lightingDesc, IID_PPV_ARGS(&m_directionalPipeline)),
        "Create directional light pipeline state");

    auto& additiveTarget = lightingDesc.BlendState.RenderTarget[0];
    additiveTarget.BlendEnable = TRUE;
    additiveTarget.SrcBlend = D3D12_BLEND_ONE;
    additiveTarget.DestBlend = D3D12_BLEND_ONE;
    additiveTarget.BlendOp = D3D12_BLEND_OP_ADD;
    additiveTarget.SrcBlendAlpha = D3D12_BLEND_ONE;
    additiveTarget.DestBlendAlpha = D3D12_BLEND_ONE;
    additiveTarget.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    dx::Check(
        m_device->CreateGraphicsPipelineState(&lightingDesc, IID_PPV_ARGS(&m_additiveLightPipeline)),
        "Create additive light pipeline state");
}

void RenderingSystem::LoadScene()
{
    const auto objPath = dx::ExecutableDirectory() / "assets" / "Sponza-master" / "sponza.obj";
    m_scene.Load(m_device.Get(), m_commandList.Get(), objPath);
}

void RenderingSystem::CreateLights()
{
    m_directionalLight.colorIntensity = {1.0f, 0.94f, 0.84f, 1.10f};
    m_directionalLight.directionInnerCos = {-0.35f, -1.0f, 0.20f, 0.0f};
    m_directionalLight.parameters = {0.0f, 0.0f, 0.075f, 0.0f};

    const std::array<XMFLOAT3, 8> positions = {{
        {-760.0f, 160.0f, -120.0f},
        {-520.0f, 190.0f, 210.0f},
        {-250.0f, 135.0f, -180.0f},
        {0.0f, 150.0f, 120.0f},
        {260.0f, 140.0f, -170.0f},
        {520.0f, 190.0f, 220.0f},
        {760.0f, 165.0f, -90.0f},
        {0.0f, 430.0f, 0.0f},
    }};
    const std::array<XMFLOAT3, 8> colors = {{
        {1.0f, 0.25f, 0.18f},
        {0.20f, 0.45f, 1.0f},
        {1.0f, 0.70f, 0.18f},
        {0.20f, 1.0f, 0.45f},
        {0.75f, 0.25f, 1.0f},
        {0.18f, 0.75f, 1.0f},
        {1.0f, 0.30f, 0.20f},
        {0.85f, 0.90f, 1.0f},
    }};

    m_pointLights.clear();
    for (std::size_t i = 0; i < positions.size(); ++i)
    {
        LightConstants light{};
        light.colorIntensity = {colors[i].x, colors[i].y, colors[i].z, i == positions.size() - 1 ? 5.0f : 8.0f};
        light.positionRange = {positions[i].x, positions[i].y, positions[i].z, 420.0f};
        light.parameters = {0.0f, 1.0f, 0.0f, 0.0f};
        m_pointLights.push_back(light);
    }

    const float innerCosine = std::cos(XMConvertToRadians(17.0f));
    const float outerCosine = std::cos(XMConvertToRadians(27.0f));
    m_spotLights.clear();

    LightConstants warmSpot{};
    warmSpot.colorIntensity = {1.0f, 0.72f, 0.38f, 11.0f};
    warmSpot.positionRange = {-520.0f, 650.0f, 0.0f, 920.0f};
    warmSpot.directionInnerCos = {0.18f, -1.0f, 0.06f, innerCosine};
    warmSpot.parameters = {outerCosine, 2.0f, 0.0f, 0.0f};
    m_spotLights.push_back(warmSpot);

    LightConstants coolSpot{};
    coolSpot.colorIntensity = {0.35f, 0.60f, 1.0f, 11.0f};
    coolSpot.positionRange = {520.0f, 650.0f, 0.0f, 920.0f};
    coolSpot.directionInnerCos = {-0.18f, -1.0f, -0.06f, innerCosine};
    coolSpot.parameters = {outerCosine, 2.0f, 0.0f, 0.0f};
    m_spotLights.push_back(coolSpot);
}

void RenderingSystem::Update(float deltaSeconds)
{
    m_elapsedSeconds += deltaSeconds;
    m_camera.Update(deltaSeconds, m_keys);

    for (std::size_t i = 0; i < m_pointLights.size(); ++i)
    {
        const float phase = m_elapsedSeconds * 1.3f + static_cast<float>(i) * 0.8f;
        m_pointLights[i].colorIntensity.w = (i == m_pointLights.size() - 1 ? 4.5f : 7.0f) + std::sin(phase) * 1.0f;
    }
}

RenderingSystem::FrameResource& RenderingSystem::BeginFrame()
{
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    FrameResource& frame = m_frames[m_frameIndex];

    if (frame.fenceValue != 0 && m_fence->GetCompletedValue() < frame.fenceValue)
    {
        dx::Check(m_fence->SetEventOnCompletion(frame.fenceValue, m_fenceEvent), "Set frame fence event");
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    dx::Check(frame.commandAllocator->Reset(), "Reset frame command allocator");
    dx::Check(m_commandList->Reset(frame.commandAllocator.Get(), nullptr), "Reset command list");
    return frame;
}

void RenderingSystem::Render()
{
    FrameResource& frame = BeginFrame();

    const XMMATRIX viewProjection = m_camera.View() * m_camera.Projection();
    FrameConstants frameConstants{};
    XMStoreFloat4x4(&frameConstants.viewProjection, XMMatrixTranspose(viewProjection));

    LightingConstants lightingConstants{};
    XMStoreFloat4x4(&lightingConstants.inverseViewProjection, XMMatrixTranspose(XMMatrixInverse(nullptr, viewProjection)));
    lightingConstants.cameraPosition = m_camera.Position();
    lightingConstants.screenSize = {static_cast<float>(m_width), static_cast<float>(m_height)};
    lightingConstants.inverseScreenSize = {1.0f / m_width, 1.0f / m_height};

    std::memcpy(frame.mappedConstants + m_frameConstantsOffset, &frameConstants, sizeof(frameConstants));
    std::memcpy(frame.mappedConstants + m_lightingConstantsOffset, &lightingConstants, sizeof(lightingConstants));

    m_commandList->RSSetViewports(1, &m_viewport);
    m_commandList->RSSetScissorRects(1, &m_scissor);

    m_gBuffer.TransitionToGeometry(m_commandList.Get());
    m_gBuffer.ClearAndBind(m_commandList.Get());

    ID3D12DescriptorHeap* materialHeaps[] = {m_scene.TextureHeap()};
    m_commandList->SetDescriptorHeaps(1, materialHeaps);
    m_commandList->SetGraphicsRootSignature(m_geometryRootSignature.Get());
    m_commandList->SetPipelineState(m_geometryPipeline.Get());
    m_commandList->SetGraphicsRootConstantBufferView(
        0,
        frame.constantBuffer->GetGPUVirtualAddress() + m_frameConstantsOffset);
    m_scene.Draw(m_commandList.Get(), 1, 2);

    m_gBuffer.TransitionToLighting(m_commandList.Get());

    const auto toRenderTarget = dx::TransitionBarrier(
        m_backBuffers[m_frameIndex].Get(),
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_commandList->ResourceBarrier(1, &toRenderTarget);

    auto backBufferRtv = m_backBufferRtvHeap->GetCPUDescriptorHandleForHeapStart();
    backBufferRtv.ptr += static_cast<SIZE_T>(m_frameIndex) * m_backBufferRtvIncrement;
    constexpr float black[4] = {0.005f, 0.008f, 0.012f, 1.0f};
    m_commandList->ClearRenderTargetView(backBufferRtv, black, 0, nullptr);
    m_commandList->OMSetRenderTargets(1, &backBufferRtv, FALSE, nullptr);

    ID3D12DescriptorHeap* lightingHeaps[] = {m_gBuffer.ShaderVisibleHeap()};
    m_commandList->SetDescriptorHeaps(1, lightingHeaps);
    m_commandList->SetGraphicsRootSignature(m_lightingRootSignature.Get());
    m_commandList->SetGraphicsRootDescriptorTable(0, m_gBuffer.SrvTable());
    m_commandList->SetGraphicsRootConstantBufferView(
        1,
        frame.constantBuffer->GetGPUVirtualAddress() + m_lightingConstantsOffset);
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    m_commandList->SetPipelineState(m_directionalPipeline.Get());
    m_commandList->SetGraphicsRoot32BitConstants(2, 16, &m_directionalLight, 0);
    m_commandList->DrawInstanced(3, 1, 0, 0);

    m_commandList->SetPipelineState(m_additiveLightPipeline.Get());
    for (const auto& light : m_pointLights)
    {
        m_commandList->SetGraphicsRoot32BitConstants(2, 16, &light, 0);
        m_commandList->DrawInstanced(3, 1, 0, 0);
    }
    for (const auto& light : m_spotLights)
    {
        m_commandList->SetGraphicsRoot32BitConstants(2, 16, &light, 0);
        m_commandList->DrawInstanced(3, 1, 0, 0);
    }

    const auto toPresent = dx::TransitionBarrier(
        m_backBuffers[m_frameIndex].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);
    m_commandList->ResourceBarrier(1, &toPresent);
    EndFrame(frame);
}

void RenderingSystem::EndFrame(FrameResource& frame)
{
    dx::Check(m_commandList->Close(), "Close frame command list");
    ID3D12CommandList* lists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, lists);
    dx::Check(m_swapChain->Present(1, 0), "Present");

    frame.fenceValue = m_nextFenceValue++;
    dx::Check(m_commandQueue->Signal(m_fence.Get(), frame.fenceValue), "Signal frame fence");
}

void RenderingSystem::FlushGpu()
{
    const UINT64 value = m_nextFenceValue++;
    dx::Check(m_commandQueue->Signal(m_fence.Get(), value), "Signal flush fence");
    if (m_fence->GetCompletedValue() < value)
    {
        dx::Check(m_fence->SetEventOnCompletion(value, m_fenceEvent), "Set flush fence event");
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void RenderingSystem::Resize(UINT width, UINT height)
{
    if (!m_initialized || width == 0 || height == 0 || (width == m_width && height == m_height))
    {
        return;
    }

    FlushGpu();
    for (auto& buffer : m_backBuffers)
    {
        buffer.Reset();
    }
    for (auto& frame : m_frames)
    {
        frame.fenceValue = 0;
    }

    dx::Check(
        m_swapChain->ResizeBuffers(FrameCount, width, height, BackBufferFormat, 0),
        "Resize swap chain buffers");
    m_width = width;
    m_height = height;
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    CreateBackBufferViews();
    m_gBuffer.Resize(m_device.Get(), width, height);

    m_viewport.Width = static_cast<float>(width);
    m_viewport.Height = static_cast<float>(height);
    m_scissor.right = static_cast<LONG>(width);
    m_scissor.bottom = static_cast<LONG>(height);
    m_camera.SetAspect(static_cast<float>(width) / static_cast<float>(height));
}

LRESULT RenderingSystem::HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_SIZE:
        m_minimized = wParam == SIZE_MINIMIZED;
        if (!m_minimized)
        {
            Resize(LOWORD(lParam), HIWORD(lParam));
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam < m_keys.size()) m_keys[wParam] = true;
        if (wParam == VK_ESCAPE) DestroyWindow(window);
        return 0;

    case WM_KEYUP:
        if (wParam < m_keys.size()) m_keys[wParam] = false;
        return 0;

    case WM_LBUTTONDOWN:
        m_mouseLook = true;
        m_lastMouse = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        SetCapture(window);
        return 0;

    case WM_LBUTTONUP:
        m_mouseLook = false;
        ReleaseCapture();
        return 0;

    case WM_MOUSEMOVE:
        if (m_mouseLook)
        {
            const POINT current{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            m_camera.Rotate(
                static_cast<float>(current.x - m_lastMouse.x),
                static_cast<float>(current.y - m_lastMouse.y));
            m_lastMouse = current;
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

LRESULT CALLBACK RenderingSystem::WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    RenderingSystem* application = nullptr;
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        application = static_cast<RenderingSystem*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(application));
    }
    else
    {
        application = reinterpret_cast<RenderingSystem*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    return application
        ? application->HandleMessage(window, message, wParam, lParam)
        : DefWindowProcW(window, message, wParam, lParam);
}
