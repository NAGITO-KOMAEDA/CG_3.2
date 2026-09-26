#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
using namespace DirectX;

struct Vertex {
    XMFLOAT3 position;
    XMFLOAT3 normal;
    XMFLOAT4 tangent;
    XMFLOAT2 uv;
};

struct alignas(16) FrameConstants {
    XMFLOAT4X4 viewProjection;
    XMFLOAT4 cameraPosition;
    XMFLOAT4 lightDirection;
    XMFLOAT4 tessellation;
    XMFLOAT4 display;
};

struct alignas(16) MaterialConstants {
    XMFLOAT4 material;
};

struct Material {
    std::string name;
    fs::path diffuseFile;
    fs::path normalFile;
    fs::path heightFile;
    std::vector<Vertex> vertices;
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11ShaderResourceView> diffuse;
    ComPtr<ID3D11ShaderResourceView> normal;
    ComPtr<ID3D11ShaderResourceView> height;
    float displacement = 0.0f;
    bool hasNormal = false;
};

static void Check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char message[256];
        std::snprintf(message, sizeof(message), "%s failed (HRESULT 0x%08X)", what,
                      static_cast<unsigned>(hr));
        throw std::runtime_error(message);
    }
}

static XMFLOAT3 Add(XMFLOAT3 a, XMFLOAT3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
static XMFLOAT3 Sub(XMFLOAT3 a, XMFLOAT3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
static XMFLOAT3 Mul(XMFLOAT3 a, float s) { return {a.x*s,a.y*s,a.z*s}; }
static float Dot(XMFLOAT3 a, XMFLOAT3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
static XMFLOAT3 Cross(XMFLOAT3 a, XMFLOAT3 b) {
    return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
static XMFLOAT3 Normalize(XMFLOAT3 a) {
    const float length = std::sqrt(Dot(a,a));
    return length > 1e-8f ? Mul(a,1.0f/length) : XMFLOAT3{0,1,0};
}

static std::string Trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

static fs::path FindRoot() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    for (fs::path path : {fs::current_path(), fs::path(exe).parent_path()}) {
        for (int i=0; i<5; ++i, path=path.parent_path()) {
            if (fs::exists(path / "shaders" / "sponza.hlsl") &&
                fs::exists(path / "assets" / "Sponza-master" / "sponza.obj")) return path;
            if (path == path.parent_path()) break;
        }
    }
    throw std::runtime_error("Cannot find shaders/sponza.hlsl and assets/Sponza-master/sponza.obj");
}

static std::vector<unsigned char> ReadTga(const fs::path& file, UINT& width, UINT& height) {
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open texture: " + file.string());
    unsigned char header[18]{};
    input.read(reinterpret_cast<char*>(header), 18);
    if (!input || header[1] != 0 || header[2] != 2 ||
        (header[16] != 24 && header[16] != 32))
        throw std::runtime_error("Expected uncompressed 24/32-bit TGA: " + file.string());
    width = header[12] | (header[13] << 8);
    height = header[14] | (header[15] << 8);
    if (!width || !height || width > 8192 || height > 8192)
        throw std::runtime_error("Invalid TGA dimensions: " + file.string());
    input.seekg(header[0], std::ios::cur); // Skip image ID.
    const int channels = header[16] / 8;
    std::vector<unsigned char> source(static_cast<size_t>(width) * height * channels);
    input.read(reinterpret_cast<char*>(source.data()), source.size());
    if (!input) throw std::runtime_error("Truncated TGA: " + file.string());
    std::vector<unsigned char> rgba(static_cast<size_t>(width) * height * 4);
    const bool top = (header[17] & 0x20) != 0;
    const bool right = (header[17] & 0x10) != 0;
    for (UINT y=0; y<height; ++y) for (UINT x=0; x<width; ++x) {
        const UINT srcY = top ? y : height - 1 - y;
        const UINT srcX = right ? width - 1 - x : x;
        const size_t src = (static_cast<size_t>(srcY)*width+srcX)*channels;
        const size_t dst = (static_cast<size_t>(y)*width+x)*4;
        rgba[dst] = source[src+2];
        rgba[dst+1] = source[src+1];
        rgba[dst+2] = source[src];
        rgba[dst+3] = channels == 4 ? source[src+3] : 255;
    }
    return rgba;
}

struct FaceIndex { int p=0, t=0, n=0; };

struct QuantizedPoint {
    int32_t x=0,y=0,z=0;
    bool operator==(const QuantizedPoint& other) const {
        return x==other.x && y==other.y && z==other.z;
    }
    bool operator<(const QuantizedPoint& other) const {
        if (x!=other.x) return x<other.x;
        if (y!=other.y) return y<other.y;
        return z<other.z;
    }
};
struct TriangleKey {
    std::array<QuantizedPoint,3> points;
    bool operator==(const TriangleKey& other) const { return points==other.points; }
};
struct TriangleHash {
    size_t operator()(const TriangleKey& key) const {
        size_t hash=1469598103934665603ull;
        for (const auto& p:key.points) for (int32_t value:{p.x,p.y,p.z}) {
            hash^=static_cast<uint32_t>(value);
            hash*=1099511628211ull;
        }
        return hash;
    }
};
static TriangleKey PositionKey(const Vertex (&v)[3]) {
    TriangleKey key{};
    for (int i=0;i<3;++i) {
        const auto& p=v[i].position;
        key.points[i]={static_cast<int32_t>(std::lround(p.x*100000.0f)),
                       static_cast<int32_t>(std::lround(p.y*100000.0f)),
                       static_cast<int32_t>(std::lround(p.z*100000.0f))};
    }
    std::sort(key.points.begin(),key.points.end());
    return key;
}

static FaceIndex ParseIndex(const std::string& text) {
    FaceIndex result;
    std::sscanf(text.c_str(), "%d/%d/%d", &result.p, &result.t, &result.n);
    return result;
}
static int Resolve(int index, size_t count) { return index > 0 ? index - 1 : static_cast<int>(count) + index; }

class App {
public:
    void SetCamera(float x, float y, float z, float heading) {
        camera={x,y,z}; yaw=heading;
    }
    void SetLevelView(bool enabled) { showLevels=enabled; }
    void Run(bool smokeTest=false) {
        root = FindRoot();
        smoke = smokeTest;
        CreateWindowAndDevice();
        CreateShaders();
        CreateStates();
        LoadScene();
        if (smoke) { Render(); return; }
        auto last = std::chrono::steady_clock::now();
        MSG message{};
        while (message.message != WM_QUIT) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (message.message == WM_QUIT) break;
            const auto now = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(now-last).count();
            last = now;
            Update(std::min(dt, 0.1f));
            Render();
        }
    }

    void MouseMove(int x, int y, WPARAM buttons) {
        if (buttons & MK_LBUTTON) {
            yaw += (x-lastMouseX)*0.004f;
            pitch = std::clamp(pitch-(y-lastMouseY)*0.004f,-1.4f,1.4f);
        }
        lastMouseX=x; lastMouseY=y;
    }

private:
    fs::path root;
    HWND window=nullptr;
    UINT width=1280, height=720;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapChain;
    ComPtr<ID3D11Texture2D> msaaColor;
    ComPtr<ID3D11RenderTargetView> colorTarget;
    ComPtr<ID3D11DepthStencilView> depthTarget;
    UINT msaaSamples=1, msaaQuality=0;
    ComPtr<ID3D11VertexShader> plainVS, controlVS;
    ComPtr<ID3D11HullShader> hullShader;
    ComPtr<ID3D11DomainShader> domainShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    ComPtr<ID3D11InputLayout> inputLayout;
    ComPtr<ID3D11Buffer> frameBuffer, materialBuffer;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> solidState, wireState;
    ComPtr<ID3D11ShaderResourceView> whiteTexture, flatNormal, neutralHeight;
    std::unordered_map<std::wstring,ComPtr<ID3D11ShaderResourceView>> textureCache;
    std::vector<Material> materials;
    std::unordered_map<std::string,size_t> materialIndex;
    XMFLOAT3 camera{-4.0f,2.4f,0.0f};
    float yaw=1.5707963f, pitch=0.0f, maximumTess=10.0f;
    bool tessEnabled=true, wireframe=false, showLevels=false;
    bool smoke=false;
    int lastMouseX=0,lastMouseY=0;
    double titleTimer=0.0;

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        App* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        switch(msg) {
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        case WM_LBUTTONDOWN: SetCapture(hwnd); return 0;
        case WM_LBUTTONUP: ReleaseCapture(); return 0;
        case WM_MOUSEMOVE:
            if (self) self->MouseMove(GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam),wParam);
            return 0;
        }
        return DefWindowProcW(hwnd,msg,wParam,lParam);
    }

    void CreateWindowAndDevice() {
        WNDCLASSW klass{};
        klass.lpfnWndProc=WindowProc;
        klass.hInstance=GetModuleHandleW(nullptr);
        klass.lpszClassName=L"SponzaTessellationLab";
        klass.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        Check(RegisterClassW(&klass) ? S_OK : HRESULT_FROM_WIN32(GetLastError()),"RegisterClassW");
        RECT rect{0,0,static_cast<LONG>(width),static_cast<LONG>(height)};
        AdjustWindowRect(&rect,WS_OVERLAPPEDWINDOW,FALSE);
        window=CreateWindowW(klass.lpszClassName,L"Sponza tessellation",WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT,CW_USEDEFAULT,rect.right-rect.left,rect.bottom-rect.top,
                             nullptr,nullptr,klass.hInstance,nullptr);
        if (!window) throw std::runtime_error("CreateWindowW failed");
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(this));
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferCount=1;
        desc.BufferDesc.Width=width;
        desc.BufferDesc.Height=height;
        desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow=window;
        desc.SampleDesc.Count=1;
        desc.Windowed=TRUE;
        desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
        D3D_FEATURE_LEVEL level{};
        const D3D_FEATURE_LEVEL requested[]{D3D_FEATURE_LEVEL_11_0};
        HRESULT hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,
            requested,1,D3D11_SDK_VERSION,&desc,swapChain.GetAddressOf(),
            device.GetAddressOf(),&level,context.GetAddressOf());
        if (FAILED(hr)) hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,
            requested,1,D3D11_SDK_VERSION,&desc,swapChain.GetAddressOf(),
            device.GetAddressOf(),&level,context.GetAddressOf());
        Check(hr,"D3D11CreateDeviceAndSwapChain (feature level 11.0)");
        UINT colorQuality=0,depthQuality=0;
        device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM,4,&colorQuality);
        device->CheckMultisampleQualityLevels(DXGI_FORMAT_D24_UNORM_S8_UINT,4,&depthQuality);
        if (colorQuality && depthQuality) {
            msaaSamples=4;
            msaaQuality=std::min(colorQuality,depthQuality)-1;
        }
        CreateTargets();
        if (!smoke) ShowWindow(window,SW_SHOW);
    }

    void CreateTargets() {
        colorTarget.Reset(); depthTarget.Reset(); msaaColor.Reset();
        if (msaaSamples>1) {
            D3D11_TEXTURE2D_DESC colorDesc{};
            colorDesc.Width=width; colorDesc.Height=height;
            colorDesc.MipLevels=1; colorDesc.ArraySize=1;
            colorDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
            colorDesc.SampleDesc.Count=msaaSamples;
            colorDesc.SampleDesc.Quality=msaaQuality;
            colorDesc.Usage=D3D11_USAGE_DEFAULT;
            colorDesc.BindFlags=D3D11_BIND_RENDER_TARGET;
            Check(device->CreateTexture2D(&colorDesc,nullptr,msaaColor.GetAddressOf()),
                  "Create MSAA color texture");
            Check(device->CreateRenderTargetView(msaaColor.Get(),nullptr,colorTarget.GetAddressOf()),
                  "Create MSAA render target");
        } else {
            ComPtr<ID3D11Texture2D> back;
            Check(swapChain->GetBuffer(0,IID_PPV_ARGS(back.GetAddressOf())),"GetBuffer");
            Check(device->CreateRenderTargetView(back.Get(),nullptr,colorTarget.GetAddressOf()),
                  "CreateRenderTargetView");
        }
        D3D11_TEXTURE2D_DESC depthDesc{};
        depthDesc.Width=width; depthDesc.Height=height;
        depthDesc.MipLevels=1; depthDesc.ArraySize=1;
        depthDesc.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthDesc.SampleDesc.Count=msaaSamples;
        depthDesc.SampleDesc.Quality=msaaQuality;
        depthDesc.Usage=D3D11_USAGE_DEFAULT;
        depthDesc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> depth;
        Check(device->CreateTexture2D(&depthDesc,nullptr,depth.GetAddressOf()),"Create depth texture");
        Check(device->CreateDepthStencilView(depth.Get(),nullptr,depthTarget.GetAddressOf()),
              "CreateDepthStencilView");
    }

    ComPtr<ID3DBlob> Compile(const char* entry, const char* target) {
        ComPtr<ID3DBlob> code, errors;
        const auto path=(root / "shaders" / "sponza.hlsl").wstring();
        const HRESULT hr=D3DCompileFromFile(path.c_str(),nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entry,target,D3DCOMPILE_ENABLE_STRICTNESS,0,code.GetAddressOf(),errors.GetAddressOf());
        if (FAILED(hr)) {
            std::string message=errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize()) : "Unknown HLSL error";
            throw std::runtime_error(message);
        }
        return code;
    }

    void CreateShaders() {
        auto plainCode=Compile("VSPlain","vs_5_0");
        auto controlCode=Compile("VSControl","vs_5_0");
        auto hullCode=Compile("HSMain","hs_5_0");
        auto domainCode=Compile("DSMain","ds_5_0");
        auto pixelCode=Compile("PSMain","ps_5_0");
        Check(device->CreateVertexShader(plainCode->GetBufferPointer(),plainCode->GetBufferSize(),
            nullptr,plainVS.GetAddressOf()),"Create plain VS");
        Check(device->CreateVertexShader(controlCode->GetBufferPointer(),controlCode->GetBufferSize(),
            nullptr,controlVS.GetAddressOf()),"Create control VS");
        Check(device->CreateHullShader(hullCode->GetBufferPointer(),hullCode->GetBufferSize(),
            nullptr,hullShader.GetAddressOf()),"Create hull shader");
        Check(device->CreateDomainShader(domainCode->GetBufferPointer(),domainCode->GetBufferSize(),
            nullptr,domainShader.GetAddressOf()),"Create domain shader");
        Check(device->CreatePixelShader(pixelCode->GetBufferPointer(),pixelCode->GetBufferSize(),
            nullptr,pixelShader.GetAddressOf()),"Create pixel shader");
        D3D11_INPUT_ELEMENT_DESC elements[]{
            {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,offsetof(Vertex,position),D3D11_INPUT_PER_VERTEX_DATA,0},
            {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,offsetof(Vertex,normal),D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TANGENT",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,offsetof(Vertex,tangent),D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,offsetof(Vertex,uv),D3D11_INPUT_PER_VERTEX_DATA,0},
        };
        Check(device->CreateInputLayout(elements,4,plainCode->GetBufferPointer(),
            plainCode->GetBufferSize(),inputLayout.GetAddressOf()),"CreateInputLayout");
    }

    ComPtr<ID3D11ShaderResourceView> Texture(const std::vector<unsigned char>& pixels,
                                               UINT w, UINT h, bool srgb) {
        // Without mipmaps, the large tiled floor texture shimmers as the
        // camera moves. Build the entire chain once while loading the TGA.
        std::vector<std::vector<unsigned char>> levels;
        levels.push_back(pixels);
        UINT previousWidth=w, previousHeight=h;
        while (previousWidth>1 || previousHeight>1) {
            const UINT nextWidth=std::max(1u,previousWidth/2);
            const UINT nextHeight=std::max(1u,previousHeight/2);
            const auto& previous=levels.back();
            std::vector<unsigned char> next(static_cast<size_t>(nextWidth)*nextHeight*4);
            for (UINT y=0; y<nextHeight; ++y) for (UINT x=0; x<nextWidth; ++x) {
                for (int channel=0; channel<4; ++channel) {
                    unsigned sum=0;
                    for (UINT dy=0; dy<2; ++dy) for (UINT dx=0; dx<2; ++dx) {
                        const UINT sx=std::min(previousWidth-1,2*x+dx);
                        const UINT sy=std::min(previousHeight-1,2*y+dy);
                        sum+=previous[(static_cast<size_t>(sy)*previousWidth+sx)*4+channel];
                    }
                    next[(static_cast<size_t>(y)*nextWidth+x)*4+channel]=
                        static_cast<unsigned char>((sum+2)/4);
                }
            }
            levels.push_back(std::move(next));
            previousWidth=nextWidth;
            previousHeight=nextHeight;
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width=w; desc.Height=h;
        desc.MipLevels=static_cast<UINT>(levels.size()); desc.ArraySize=1;
        desc.Format=srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count=1; desc.Usage=D3D11_USAGE_IMMUTABLE;
        desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        std::vector<D3D11_SUBRESOURCE_DATA> data(levels.size());
        UINT levelWidth=w;
        for (size_t i=0;i<levels.size();++i) {
            data[i].pSysMem=levels[i].data();
            data[i].SysMemPitch=levelWidth*4;
            levelWidth=std::max(1u,levelWidth/2);
        }
        ComPtr<ID3D11Texture2D> texture;
        Check(device->CreateTexture2D(&desc,data.data(),texture.GetAddressOf()),"Create texture");
        ComPtr<ID3D11ShaderResourceView> view;
        Check(device->CreateShaderResourceView(texture.Get(),nullptr,view.GetAddressOf()),
              "CreateShaderResourceView");
        return view;
    }

    ComPtr<ID3D11ShaderResourceView> LoadTexture(const fs::path& path, bool srgb,
                                                   ID3D11ShaderResourceView* fallback) {
        if (path.empty() || !fs::exists(path)) return ComPtr<ID3D11ShaderResourceView>(fallback);
        const std::wstring key=path.wstring() + (srgb ? L"#srgb" : L"#linear");
        auto found=textureCache.find(key);
        if (found!=textureCache.end()) return found->second;
        UINT w=0,h=0;
        auto pixels=ReadTga(path,w,h);
        auto view=Texture(pixels,w,h,srgb);
        textureCache.emplace(key,view);
        return view;
    }

    void CreateStates() {
        D3D11_BUFFER_DESC cb{};
        cb.Usage=D3D11_USAGE_DEFAULT; cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        cb.ByteWidth=sizeof(FrameConstants);
        Check(device->CreateBuffer(&cb,nullptr,frameBuffer.GetAddressOf()),"Create frame buffer");
        cb.ByteWidth=sizeof(MaterialConstants);
        Check(device->CreateBuffer(&cb,nullptr,materialBuffer.GetAddressOf()),"Create material buffer");
        D3D11_SAMPLER_DESC sd{};
        sd.Filter=D3D11_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy=8;
        sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MaxLOD=D3D11_FLOAT32_MAX;
        Check(device->CreateSamplerState(&sd,sampler.GetAddressOf()),"CreateSamplerState");
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode=D3D11_FILL_SOLID;
        rd.CullMode=D3D11_CULL_NONE; // Sponza has double-sided fabric and foliage.
        rd.DepthClipEnable=TRUE;
        Check(device->CreateRasterizerState(&rd,solidState.GetAddressOf()),"Create solid rasterizer");
        rd.FillMode=D3D11_FILL_WIREFRAME;
        Check(device->CreateRasterizerState(&rd,wireState.GetAddressOf()),"Create wire rasterizer");
        whiteTexture=Texture({255,255,255,255},1,1,true);
        flatNormal=Texture({128,128,255,255},1,1,false);
        neutralHeight=Texture({128,128,128,255},1,1,false);
    }

    size_t MaterialId(const std::string& name) {
        auto it=materialIndex.find(name);
        if (it!=materialIndex.end()) return it->second;
        const size_t id=materials.size();
        materialIndex[name]=id;
        materials.emplace_back();
        materials.back().name=name;
        return id;
    }

    void LoadMtl(const fs::path& path) {
        std::ifstream input(path);
        if (!input) throw std::runtime_error("Cannot read sponza.mtl");
        Material* current=nullptr;
        std::string line;
        while (std::getline(input,line)) {
            line=Trim(line);
            if (line.rfind("newmtl ",0)==0) current=&materials[MaterialId(Trim(line.substr(7)))];
            else if (current && line.rfind("map_Kd ",0)==0)
                current->diffuseFile=path.parent_path()/Trim(line.substr(7));
            else if (current && line.rfind("map_Disp ",0)==0) {
                // The supplied MTL mislabels *_ddn.tga: these are normal maps.
                current->normalFile=path.parent_path()/Trim(line.substr(9));
                const fs::path stem=current->normalFile.stem();
                current->heightFile=current->normalFile.parent_path()/
                    (stem.string()+"_height.tga");
            }
        }
    }

    void LoadObj(const fs::path& path) {
        std::ifstream input(path);
        if (!input) throw std::runtime_error("Cannot read sponza.obj");
        std::vector<XMFLOAT3> positions, normals;
        std::vector<XMFLOAT2> uvs;
        std::unordered_map<size_t,std::unordered_set<TriangleKey,TriangleHash>> seenFaces;
        std::string line;
        size_t current=MaterialId("default");
        size_t triangles=0;
        size_t duplicates=0;
        while (std::getline(input,line)) {
            if (line.rfind("v ",0)==0) {
                XMFLOAT3 p{};
                std::sscanf(line.c_str()+2,"%f %f %f",&p.x,&p.y,&p.z);
                positions.push_back(Mul(p,0.01f)); // Original OBJ units to metres.
            } else if (line.rfind("vt ",0)==0) {
                XMFLOAT2 uv{};
                std::sscanf(line.c_str()+3,"%f %f",&uv.x,&uv.y);
                uvs.push_back({uv.x,1.0f-uv.y});
            } else if (line.rfind("vn ",0)==0) {
                XMFLOAT3 n{};
                std::sscanf(line.c_str()+3,"%f %f %f",&n.x,&n.y,&n.z);
                normals.push_back(n);
            } else if (line.rfind("usemtl ",0)==0) {
                current=MaterialId(Trim(line.substr(7)));
            } else if (line.rfind("f ",0)==0) {
                std::istringstream fields(line.substr(2));
                std::vector<FaceIndex> face;
                std::string token;
                while (fields >> token) face.push_back(ParseIndex(token));
                if (face.size()<3) continue;
                for (size_t i=1;i+1<face.size();++i) {
                    FaceIndex corners[]{face[0],face[i],face[i+1]};
                    Vertex v[3]{};
                    for (int j=0;j<3;++j) {
                        const int p=Resolve(corners[j].p,positions.size());
                        const int t=Resolve(corners[j].t,uvs.size());
                        const int n=Resolve(corners[j].n,normals.size());
                        if (p<0 || p>=static_cast<int>(positions.size()))
                            throw std::runtime_error("OBJ position index out of range");
                        v[j].position=positions[p];
                        v[j].uv=t>=0 && t<static_cast<int>(uvs.size()) ? uvs[t] : XMFLOAT2{0,0};
                        v[j].normal=n>=0 && n<static_cast<int>(normals.size()) ? normals[n] : XMFLOAT3{0,1,0};
                    }
                    // Sponza contains coplanar copies of some opaque wall
                    // triangles with different UVs. They z-fight as the eye
                    // moves. Keep one copy; fabric/foliage back faces stay.
                    const std::string& name=materials[current].name;
                    const bool opaque=name.rfind("fabric",0)!=0 &&
                                      name!="leaf" && name!="chain";
                    if (opaque && !seenFaces[current].insert(PositionKey(v)).second) {
                        ++duplicates;
                        continue;
                    }
                    const XMFLOAT3 e1=Sub(v[1].position,v[0].position);
                    const XMFLOAT3 e2=Sub(v[2].position,v[0].position);
                    const float du1=v[1].uv.x-v[0].uv.x, dv1=v[1].uv.y-v[0].uv.y;
                    const float du2=v[2].uv.x-v[0].uv.x, dv2=v[2].uv.y-v[0].uv.y;
                    const float determinant=du1*dv2-du2*dv1;
                    XMFLOAT3 tangent{},bitangent{};
                    if (std::abs(determinant)>1e-8f) {
                        tangent=Mul(Sub(Mul(e1,dv2),Mul(e2,dv1)),1.0f/determinant);
                        bitangent=Mul(Sub(Mul(e2,du1),Mul(e1,du2)),1.0f/determinant);
                    }
                    for (auto& vertex:v) {
                        const XMFLOAT3 N=Normalize(vertex.normal);
                        XMFLOAT3 T=Sub(tangent,Mul(N,Dot(N,tangent)));
                        if (Dot(T,T)<1e-8f) T=Cross(std::abs(N.y)<0.9f ? XMFLOAT3{0,1,0} :
                                                       XMFLOAT3{1,0,0},N);
                        T=Normalize(T);
                        const float handedness=Dot(Cross(N,T),bitangent)<0.0f ? -1.0f : 1.0f;
                        vertex.normal=N;
                        vertex.tangent={T.x,T.y,T.z,handedness};
                        materials[current].vertices.push_back(vertex);
                    }
                    ++triangles;
                }
            }
        }
        std::cout << "Loaded " << triangles << " triangles, " << materials.size()
                  << " materials; skipped " << duplicates << " coplanar duplicates\n";
    }

    void LoadScene() {
        const fs::path model=root/"assets"/"Sponza-master";
        LoadMtl(model/"sponza.mtl");
        LoadObj(model/"sponza.obj");
        for (auto& material:materials) {
            if (material.vertices.empty()) continue;
            if (material.vertices.size()>UINT_MAX/sizeof(Vertex))
                throw std::runtime_error("Material vertex buffer too large");
            D3D11_BUFFER_DESC desc{};
            desc.Usage=D3D11_USAGE_IMMUTABLE;
            desc.BindFlags=D3D11_BIND_VERTEX_BUFFER;
            desc.ByteWidth=static_cast<UINT>(material.vertices.size()*sizeof(Vertex));
            D3D11_SUBRESOURCE_DATA data{};
            data.pSysMem=material.vertices.data();
            Check(device->CreateBuffer(&desc,&data,material.buffer.GetAddressOf()),
                  "Create vertex buffer");
            material.diffuse=LoadTexture(material.diffuseFile,true,whiteTexture.Get());
            material.normal=LoadTexture(material.normalFile,false,flatNormal.Get());
            material.height=LoadTexture(material.heightFile,false,neutralHeight.Get());
            material.hasNormal=!material.normalFile.empty() && fs::exists(material.normalFile);
            // The floor has very large, sparse source triangles. As their
            // camera-dependent tessellation changes, their displaced shape
            // visibly pops. Keep the floor flat and retain its normal map.
            material.displacement=material.name!="floor" &&
                !material.heightFile.empty() && fs::exists(material.heightFile)
                ? 0.025f : 0.0f;
            material.vertices.clear();
            material.vertices.shrink_to_fit();
        }
    }

    void Update(float dt) {
        if (GetAsyncKeyState(VK_ESCAPE)&1) PostQuitMessage(0);
        if (GetAsyncKeyState('T')&1) tessEnabled=!tessEnabled;
        if (GetAsyncKeyState('F')&1) wireframe=!wireframe;
        if (GetAsyncKeyState('L')&1) showLevels=!showLevels;
        if (GetAsyncKeyState(VK_OEM_PLUS)&1) maximumTess=std::min(32.0f,maximumTess+2.0f);
        if (GetAsyncKeyState(VK_OEM_MINUS)&1) maximumTess=std::max(2.0f,maximumTess-2.0f);
        const float speed=(GetAsyncKeyState(VK_SHIFT)&0x8000 ? 8.0f : 3.0f)*dt;
        const XMFLOAT3 forward{std::sin(yaw)*std::cos(pitch),std::sin(pitch),
                                std::cos(yaw)*std::cos(pitch)};
        const XMFLOAT3 right{std::cos(yaw),0,-std::sin(yaw)};
        if (GetAsyncKeyState('W')&0x8000) camera=Add(camera,Mul(forward,speed));
        if (GetAsyncKeyState('S')&0x8000) camera=Sub(camera,Mul(forward,speed));
        if (GetAsyncKeyState('D')&0x8000) camera=Add(camera,Mul(right,speed));
        if (GetAsyncKeyState('A')&0x8000) camera=Sub(camera,Mul(right,speed));
        if (GetAsyncKeyState('E')&0x8000) camera.y+=speed;
        if (GetAsyncKeyState('Q')&0x8000) camera.y-=speed;
        titleTimer+=dt;
        if (titleTimer>0.4) {
            wchar_t title[256];
            swprintf_s(title,L"Sponza | T: %s | F: wireframe | L: LOD colors | +/-: max %.0f | WASD + mouse",
                       tessEnabled ? L"on" : L"off",maximumTess);
            SetWindowTextW(window,title);
            titleTimer=0.0;
        }
    }

    void Render() {
        RECT client{}; GetClientRect(window,&client);
        const UINT newWidth=std::max<LONG>(0,client.right-client.left);
        const UINT newHeight=std::max<LONG>(0,client.bottom-client.top);
        if (!newWidth || !newHeight) { Sleep(30); return; }
        if (newWidth!=width || newHeight!=height) {
            context->OMSetRenderTargets(0,nullptr,nullptr);
            colorTarget.Reset(); depthTarget.Reset(); msaaColor.Reset();
            Check(swapChain->ResizeBuffers(0,newWidth,newHeight,DXGI_FORMAT_UNKNOWN,0),"ResizeBuffers");
            width=newWidth; height=newHeight;
            CreateTargets();
        }
        const float clear[]{0.11f,0.14f,0.19f,1.0f};
        context->ClearRenderTargetView(colorTarget.Get(),clear);
        context->ClearDepthStencilView(depthTarget.Get(),D3D11_CLEAR_DEPTH,1.0f,0);
        ID3D11RenderTargetView* rtv=colorTarget.Get();
        context->OMSetRenderTargets(1,&rtv,depthTarget.Get());
        D3D11_VIEWPORT viewport{0,0,static_cast<float>(width),static_cast<float>(height),0,1};
        context->RSSetViewports(1,&viewport);
        context->RSSetState(wireframe ? wireState.Get() : solidState.Get());
        context->IASetInputLayout(inputLayout.Get());

        const XMVECTOR eye=XMLoadFloat3(&camera);
        const XMVECTOR direction=XMVectorSet(std::sin(yaw)*std::cos(pitch),std::sin(pitch),
                                              std::cos(yaw)*std::cos(pitch),0);
        const XMMATRIX view=XMMatrixLookToLH(eye,direction,XMVectorSet(0,1,0,0));
        // Sponza spans roughly 40 m. A tighter near/far range gives the lion
        // relief and the wall behind it more reliable depth separation.
        const XMMATRIX projection=XMMatrixPerspectiveFovLH(XM_PIDIV4,
            static_cast<float>(width)/height,0.2f,80.0f);
        FrameConstants frame{};
        XMStoreFloat4x4(&frame.viewProjection,XMMatrixTranspose(view*projection));
        frame.cameraPosition={camera.x,camera.y,camera.z,1};
        frame.lightDirection={-0.45f,-0.8f,0.35f,0};
        frame.tessellation={maximumTess,1.0f,18.0f,tessEnabled ? 1.0f : 0.0f};
        frame.display={wireframe ? 1.0f : 0.0f,showLevels ? 1.0f : 0.0f,0,0};
        context->UpdateSubresource(frameBuffer.Get(),0,nullptr,&frame,0,0);
        ID3D11Buffer* frameCB=frameBuffer.Get();
        ID3D11Buffer* materialCB=materialBuffer.Get();
        context->VSSetConstantBuffers(0,1,&frameCB);
        context->HSSetConstantBuffers(0,1,&frameCB);
        context->DSSetConstantBuffers(0,1,&frameCB);
        context->PSSetConstantBuffers(0,1,&frameCB);
        context->HSSetConstantBuffers(1,1,&materialCB);
        context->DSSetConstantBuffers(1,1,&materialCB);
        context->PSSetConstantBuffers(1,1,&materialCB);
        ID3D11SamplerState* samplers[]{sampler.Get()};
        context->DSSetSamplers(0,1,samplers);
        context->PSSetSamplers(0,1,samplers);
        context->PSSetShader(pixelShader.Get(),nullptr,0);

        for (const auto& mat:materials) {
            if (!mat.buffer) continue;
            const bool tessellated=mat.displacement>0.0f;
            if (tessellated) {
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
                context->VSSetShader(controlVS.Get(),nullptr,0);
                context->HSSetShader(hullShader.Get(),nullptr,0);
                context->DSSetShader(domainShader.Get(),nullptr,0);
            } else {
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context->VSSetShader(plainVS.Get(),nullptr,0);
                context->HSSetShader(nullptr,nullptr,0);
                context->DSSetShader(nullptr,nullptr,0);
            }
            MaterialConstants m{{mat.displacement,mat.hasNormal ? 1.0f : 0.0f,0,0}};
            context->UpdateSubresource(materialBuffer.Get(),0,nullptr,&m,0,0);
            ID3D11ShaderResourceView* textures[]{mat.diffuse.Get(),mat.normal.Get(),mat.height.Get()};
            context->DSSetShaderResources(0,3,textures);
            context->PSSetShaderResources(0,3,textures);
            UINT stride=sizeof(Vertex),offset=0;
            ID3D11Buffer* vb=mat.buffer.Get();
            context->IASetVertexBuffers(0,1,&vb,&stride,&offset);
            D3D11_BUFFER_DESC description{};
            mat.buffer->GetDesc(&description);
            context->Draw(description.ByteWidth/sizeof(Vertex),0);
        }
        if (msaaSamples>1) {
            context->OMSetRenderTargets(0,nullptr,nullptr);
            ComPtr<ID3D11Texture2D> back;
            Check(swapChain->GetBuffer(0,IID_PPV_ARGS(back.GetAddressOf())),"Get resolve buffer");
            context->ResolveSubresource(back.Get(),0,msaaColor.Get(),0,
                                        DXGI_FORMAT_R8G8B8A8_UNORM);
        }
        if (smoke) SaveScreenshot();
        Check(swapChain->Present(smoke ? 0 : 1,0),"Present");
    }

    void SaveScreenshot() {
        ComPtr<ID3D11Texture2D> back;
        Check(swapChain->GetBuffer(0,IID_PPV_ARGS(back.GetAddressOf())),"Get screenshot buffer");
        D3D11_TEXTURE2D_DESC desc{};
        back->GetDesc(&desc);
        desc.Usage=D3D11_USAGE_STAGING;
        desc.BindFlags=0;
        desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        desc.MiscFlags=0;
        ComPtr<ID3D11Texture2D> staging;
        Check(device->CreateTexture2D(&desc,nullptr,staging.GetAddressOf()),"Create screenshot texture");
        context->CopyResource(staging.Get(),back.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Map screenshot");
        std::ofstream output(root/"smoke-test.ppm",std::ios::binary);
        output << "P6\n" << width << ' ' << height << "\n255\n";
        for (UINT y=0;y<height;++y) {
            const auto* row=static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch;
            for (UINT x=0;x<width;++x) output.write(reinterpret_cast<const char*>(row+x*4),3);
        }
        context->Unmap(staging.Get(),0);
        std::cout << "Smoke test rendered: " << (root/"smoke-test.ppm") << '\n';
    }
};

int main(int argc, char** argv) {
    const bool smoke=argc>1 && std::string(argv[1])=="--smoke-test";
    try {
        App app;
        if (smoke && argc>=6) app.SetCamera(std::stof(argv[2]),std::stof(argv[3]),
                                            std::stof(argv[4]),std::stof(argv[5]));
        if (smoke && argc>=7 && std::string(argv[6])=="lod") app.SetLevelView(true);
        app.Run(smoke);
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        if (!smoke) MessageBoxA(nullptr,error.what(),"Sponza tessellation error",MB_ICONERROR);
        return 1;
    }
    return 0;
}
