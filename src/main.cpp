#include "lod.hpp"
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
using namespace DirectX;

static void check(HRESULT hr,const char* what) {
    if(FAILED(hr)) throw std::runtime_error(std::string(what)+" (HRESULT "+std::to_string(static_cast<unsigned>(hr))+")");
}

struct Vertex { float px,py,pz,nx,ny,nz,u,v; };
struct Instance { float x,y,z,scale,r,g,b,a; };
struct Material { std::string name; fs::path diffuse; ComPtr<ID3D11ShaderResourceView> texture; };
struct Batch { uint32_t first=0,count=0; int material=0; };
struct Mesh { std::vector<Vertex> vertices; std::vector<Batch> batches; Aabb bounds{}; };

static fs::path findAssets() {
    std::vector<fs::path> roots{fs::current_path()};
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr,exe,MAX_PATH);
    fs::path root=fs::path(exe).parent_path();
    for(int i=0;i<5;++i) {roots.push_back(root);if(root.has_parent_path())root=root.parent_path();}
    for(const auto& r:roots) {
        const fs::path p=r/L"assets"/L"Sponza-master";
        if(fs::exists(p/L"sponza.obj"))return p;
    }
    throw std::runtime_error("Cannot find assets/Sponza-master/sponza.obj. See README.md.");
}

static std::vector<Material> loadMaterials(const fs::path& path,std::unordered_map<std::string,int>& ids) {
    std::ifstream file(path);
    if(!file)throw std::runtime_error("Cannot open sponza.mtl");
    std::vector<Material> materials;
    materials.push_back({"default",{},{}});ids["default"]=0;
    std::string line;
    while(std::getline(file,line)) {
        std::istringstream in(line);std::string key;in>>key;
        if(key=="newmtl") {std::string name;in>>name;ids[name]=static_cast<int>(materials.size());materials.push_back({name,{},{}});}
        else if(key=="map_Kd" && materials.size()>1) {std::string filename;in>>filename;materials.back().diffuse=path.parent_path()/fs::path(filename);}
    }
    return materials;
}

struct FaceIndex { int p=0,t=0,n=0; };
static FaceIndex parseIndex(const std::string& s) {
    FaceIndex out;
    const size_t a=s.find('/'),b=s.find('/',a==std::string::npos?0:a+1);
    out.p=std::stoi(s.substr(0,a));
    if(a!=std::string::npos && b>a+1)out.t=std::stoi(s.substr(a+1,b-a-1));
    if(b!=std::string::npos && b+1<s.size())out.n=std::stoi(s.substr(b+1));
    return out;
}

static Mesh loadObj(const fs::path& path,const std::unordered_map<std::string,int>& materialIds) {
    std::ifstream file(path);
    if(!file)throw std::runtime_error("Cannot open sponza.obj");
    std::vector<XMFLOAT3> positions,normals;
    std::vector<XMFLOAT2> uv;
    Mesh mesh;
    mesh.bounds.min={1e9f,1e9f,1e9f};mesh.bounds.max={-1e9f,-1e9f,-1e9f};
    mesh.batches.push_back({0,0,0});
    std::string line;
    while(std::getline(file,line)) {
        if(line.size()<2)continue;
        if(line.compare(0,2,"v ")==0) {
            XMFLOAT3 p;std::istringstream in(line.substr(2));in>>p.x>>p.y>>p.z;
            p.x*=0.01f;p.y*=0.01f;p.z*=0.01f;
            positions.push_back(p);
            mesh.bounds.min.x=std::min(mesh.bounds.min.x,p.x);mesh.bounds.min.y=std::min(mesh.bounds.min.y,p.y);mesh.bounds.min.z=std::min(mesh.bounds.min.z,p.z);
            mesh.bounds.max.x=std::max(mesh.bounds.max.x,p.x);mesh.bounds.max.y=std::max(mesh.bounds.max.y,p.y);mesh.bounds.max.z=std::max(mesh.bounds.max.z,p.z);
        } else if(line.compare(0,3,"vt ")==0) {
            XMFLOAT2 t;std::istringstream in(line.substr(3));in>>t.x>>t.y;uv.push_back(t);
        } else if(line.compare(0,3,"vn ")==0) {
            XMFLOAT3 n;std::istringstream in(line.substr(3));in>>n.x>>n.y>>n.z;normals.push_back(n);
        } else if(line.compare(0,7,"usemtl ")==0) {
            const std::string name=line.substr(7);
            auto found=materialIds.find(name);
            const int material=found==materialIds.end()?0:found->second;
            if(mesh.batches.back().count==0)mesh.batches.back().material=material;
            else mesh.batches.push_back({static_cast<uint32_t>(mesh.vertices.size()),0,material});
        } else if(line.compare(0,2,"f ")==0) {
            std::istringstream in(line.substr(2));std::string token;
            std::vector<FaceIndex> face;
            while(in>>token)face.push_back(parseIndex(token));
            auto vertex=[&](FaceIndex i) {
                if(i.p<=0 || static_cast<size_t>(i.p)>positions.size())throw std::runtime_error("Invalid OBJ position index");
                const auto p=positions[i.p-1];
                const auto n=(i.n>0 && static_cast<size_t>(i.n)<=normals.size())?normals[i.n-1]:XMFLOAT3{0,1,0};
                const auto t=(i.t>0 && static_cast<size_t>(i.t)<=uv.size())?uv[i.t-1]:XMFLOAT2{0,0};
                return Vertex{p.x,p.y,p.z,n.x,n.y,n.z,t.x,1-t.y};
            };
            for(size_t i=1;i+1<face.size();++i) {
                mesh.vertices.push_back(vertex(face[0]));mesh.vertices.push_back(vertex(face[i]));mesh.vertices.push_back(vertex(face[i+1]));
                mesh.batches.back().count+=3;
            }
        }
    }
    mesh.batches.erase(std::remove_if(mesh.batches.begin(),mesh.batches.end(),[](const Batch& b){return b.count==0;}),mesh.batches.end());
    return mesh;
}

static std::vector<uint8_t> readTga(const fs::path& path,uint32_t& width,uint32_t& height) {
    std::ifstream file(path,std::ios::binary);
    if(!file)return {};
    uint8_t header[18]{};file.read(reinterpret_cast<char*>(header),18);
    if(!file || header[1]!=0 || header[2]!=2 || (header[16]!=24 && header[16]!=32))return {};
    width=header[12]|(header[13]<<8);height=header[14]|(header[15]<<8);
    if(width==0 || height==0 || width>8192 || height>8192)return {};
    file.seekg(header[0],std::ios::cur);
    const int channels=header[16]/8;
    std::vector<uint8_t> raw(size_t(width)*height*channels),rgba(size_t(width)*height*4);
    file.read(reinterpret_cast<char*>(raw.data()),static_cast<std::streamsize>(raw.size()));
    if(!file)return {};
    const bool topOrigin=(header[17]&0x20)!=0;
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x) {
        const size_t src=(size_t(y)*width+x)*channels;
        const size_t dst=(size_t(topOrigin?y:height-1-y)*width+x)*4;
        rgba[dst]=raw[src+2];rgba[dst+1]=raw[src+1];rgba[dst+2]=raw[src];rgba[dst+3]=channels==4?raw[src+3]:255;
    }
    return rgba;
}

static const char* shader=R"(
cbuffer Camera : register(b0) {float4x4 viewProj; float4 lightDirection; float4 cameraRight; float4 cameraUp;};
Texture2D diffuseTexture : register(t0);
SamplerState textureSampler : register(s0);
struct VSInput {float3 pos:POSITION;float3 normal:NORMAL;float2 uv:TEXCOORD0;float4 centerScale:INSTANCEPOS;float4 tint:INSTANCECOLOR;};
struct PSInput {float4 pos:SV_POSITION;float3 normal:NORMAL;float2 uv:TEXCOORD0;float4 tint:COLOR0;};
PSInput VSMain(VSInput v) {
    PSInput o;
    float3 world=v.pos*v.centerScale.w+v.centerScale.xyz;
    o.pos=mul(float4(world,1),viewProj);
    o.normal=v.normal;o.uv=v.uv;o.tint=v.tint;
    return o;
}
PSInput VSBillboard(VSInput v) {
    PSInput o;
    float3 world=v.centerScale.xyz+(cameraRight.xyz*v.pos.x+cameraUp.xyz*v.pos.y)*v.centerScale.w;
    o.pos=mul(float4(world,1),viewProj);
    o.normal=0;o.uv=v.uv;o.tint=v.tint;return o;
}
float4 PSBillboard(PSInput i):SV_TARGET {
    float4 color=diffuseTexture.Sample(textureSampler,i.uv)*i.tint;
    clip(color.a-0.15);return float4(color.rgb,1);
}
float4 PSMain(PSInput i):SV_TARGET {
    float4 albedo=diffuseTexture.Sample(textureSampler,i.uv)*i.tint;
    clip(albedo.a-0.15);
    float lighting=0.35+0.65*saturate(dot(normalize(i.normal),normalize(-lightDirection.xyz)));
    return float4(albedo.rgb*lighting,1);
}
)";

struct CameraBuffer { XMFLOAT4X4 viewProj; XMFLOAT4 lightDirection; XMFLOAT4 cameraRight,cameraUp; };

class App {
    HWND hwnd_=nullptr;
    uint32_t width_=1280,height_=720;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain> swap_;
    ComPtr<ID3D11RenderTargetView> target_;
    ComPtr<ID3D11Texture2D> depthTexture_;
    ComPtr<ID3D11DepthStencilView> depthView_;
    ComPtr<ID3D11VertexShader> vs_,billboardVs_;
    ComPtr<ID3D11PixelShader> ps_,billboardPs_;
    ComPtr<ID3D11InputLayout> layout_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11Buffer> cameraBuffer_,modelBuffer_,cubeBuffer_,instanceBuffer_;
    ComPtr<ID3D11ShaderResourceView> whiteTexture_,lionTexture_;
    ComPtr<ID3D11Buffer> quadBuffer_;
    Vec3 modelCenter_{};
    float modelRadius_=1;
    int forcedLod_=-1;
    bool selfTest_=false;
    void verifyFrame() {
        ComPtr<ID3D11Texture2D> back,staging;
        check(swap_->GetBuffer(0,IID_PPV_ARGS(&back)),"Test back buffer");
        D3D11_TEXTURE2D_DESC desc{};back->GetDesc(&desc);
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
        check(device_->CreateTexture2D(&desc,nullptr,&staging),"Test staging texture");
        context_->CopyResource(staging.Get(),back.Get());
        D3D11_MAPPED_SUBRESOURCE data{};check(context_->Map(staging.Get(),0,D3D11_MAP_READ,0,&data),"Read test frame");
        size_t pixels=0;
        for(UINT y=0;y<desc.Height;++y)for(UINT x=0;x<desc.Width;++x) {
            const auto p=static_cast<const unsigned char*>(data.pData)+y*data.RowPitch+x*4;
            if(std::abs(int(p[0])-18)>1 || std::abs(int(p[1])-26)>1 || std::abs(int(p[2])-41)>1)++pixels;
        }
        // Self-test screenshots also allow visual inspection of each representation.
        const DWORD rowBytes=(desc.Width*3+3)&~3u;
        BITMAPFILEHEADER fileHeader{};fileHeader.bfType=0x4d42;fileHeader.bfOffBits=sizeof(BITMAPFILEHEADER)+sizeof(BITMAPINFOHEADER);fileHeader.bfSize=fileHeader.bfOffBits+rowBytes*desc.Height;
        BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=desc.Width;info.biHeight=desc.Height;info.biPlanes=1;info.biBitCount=24;
        std::ofstream image("lod_"+std::to_string(forcedLod_)+".bmp",std::ios::binary);
        image.write(reinterpret_cast<const char*>(&fileHeader),sizeof(fileHeader));image.write(reinterpret_cast<const char*>(&info),sizeof(info));
        std::vector<unsigned char> row(rowBytes);
        for(UINT y=desc.Height;y>0;--y) {
            const auto source=static_cast<const unsigned char*>(data.pData)+(y-1)*data.RowPitch;
            for(UINT x=0;x<desc.Width;++x){row[x*3]=source[x*4+2];row[x*3+1]=source[x*4+1];row[x*3+2]=source[x*4];}
            image.write(reinterpret_cast<const char*>(row.data()),row.size());
        }
        context_->Unmap(staging.Get(),0);
        std::ofstream log("lod_selftest.txt",std::ios::app);
        log<<"mode="<<forcedLod_<<" non-background pixels="<<pixels<<"\n";
        if((forcedLod_==2 && pixels!=0) || (forcedLod_!=2 && pixels<100))throw std::runtime_error("LOD render verification failed");
    }
    std::vector<Material> materials_;
    Mesh model_;
    std::vector<Vertex> cube_;
    std::vector<Instance> objects_;
    std::vector<Aabb> boxes_;
    Octree tree_;
    std::vector<uint32_t> visible_;
    CullStats stats_{};
    bool culling_=true,octree_=true,keys_[256]{};
    float yaw_=0,pitch_=0;
    Vec3 camera_{0,3,-7};
    POINT lastMouse_{};
    bool mouseHeld_=false;
    std::chrono::steady_clock::time_point fpsStart_=std::chrono::steady_clock::now();
    int frames_=0;
    float fps_=0;

    static LRESULT CALLBACK windowProc(HWND hwnd,UINT message,WPARAM w,LPARAM l) {
        App* app=reinterpret_cast<App*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(message==WM_NCCREATE) {
            app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));
        }
        if(app) {
            if(message==WM_KEYDOWN) {
                if(w<256)app->keys_[w]=true;
                if(!(l&(1u<<30))) {
                    if(w==VK_F1)app->culling_=!app->culling_;
                    if(w==VK_F2)app->octree_=!app->octree_;
                    if(w==VK_F3)app->forcedLod_=(app->forcedLod_+2)%4-1;
                    if(w==VK_HOME){app->camera_={0,18,-45};app->yaw_=0;app->pitch_=-0.15f;}
                    if(w==VK_ESCAPE)PostQuitMessage(0);
                }
                return 0;
            }
            if(message==WM_KEYUP) {if(w<256)app->keys_[w]=false;return 0;}
            if(message==WM_RBUTTONDOWN) {app->mouseHeld_=true;GetCursorPos(&app->lastMouse_);SetCapture(hwnd);return 0;}
            if(message==WM_RBUTTONUP) {app->mouseHeld_=false;ReleaseCapture();return 0;}
            if(message==WM_KILLFOCUS) {std::fill(std::begin(app->keys_),std::end(app->keys_),false);app->mouseHeld_=false;return 0;}
            if(message==WM_SIZE) {app->width_=LOWORD(l);app->height_=HIWORD(l);return 0;}
        }
        if(message==WM_DESTROY) {PostQuitMessage(0);return 0;}
        return DefWindowProcW(hwnd,message,w,l);
    }

    template<class T> ComPtr<ID3D11Buffer> makeImmutableVertexBuffer(const std::vector<T>& data) {
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=static_cast<UINT>(data.size()*sizeof(T));desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=data.data();
        ComPtr<ID3D11Buffer> buffer;check(device_->CreateBuffer(&desc,&initial,&buffer),"Create vertex buffer");return buffer;
    }
    ComPtr<ID3D11ShaderResourceView> makeTexture(const uint8_t* rgba,uint32_t w,uint32_t h) {
        D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=1;desc.ArraySize=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{};data.pSysMem=rgba;data.SysMemPitch=w*4;
        ComPtr<ID3D11Texture2D> texture;check(device_->CreateTexture2D(&desc,&data,&texture),"Create texture");
        ComPtr<ID3D11ShaderResourceView> view;check(device_->CreateShaderResourceView(texture.Get(),nullptr,&view),"Create texture view");return view;
    }
    void resize() {
        if(!swap_ || !width_ || !height_)return;
        context_->OMSetRenderTargets(0,nullptr,nullptr);target_.Reset();depthView_.Reset();depthTexture_.Reset();
        check(swap_->ResizeBuffers(0,width_,height_,DXGI_FORMAT_UNKNOWN,0),"Resize swap chain");
        ComPtr<ID3D11Texture2D> back;check(swap_->GetBuffer(0,IID_PPV_ARGS(&back)),"Get back buffer");
        check(device_->CreateRenderTargetView(back.Get(),nullptr,&target_),"Create render target");
        D3D11_TEXTURE2D_DESC desc{};desc.Width=width_;desc.Height=height_;desc.MipLevels=1;desc.ArraySize=1;desc.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        check(device_->CreateTexture2D(&desc,nullptr,&depthTexture_),"Create depth buffer");
        check(device_->CreateDepthStencilView(depthTexture_.Get(),nullptr,&depthView_),"Create depth view");
        D3D11_VIEWPORT viewport{};viewport.Width=static_cast<float>(width_);viewport.Height=static_cast<float>(height_);viewport.MinDepth=0;viewport.MaxDepth=1;
        context_->RSSetViewports(1,&viewport);
    }
    void initGraphics() {
        DXGI_SWAP_CHAIN_DESC desc{};desc.BufferDesc.Width=width_;desc.BufferDesc.Height=height_;desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.OutputWindow=hwnd_;desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
        D3D_FEATURE_LEVEL level{};
        HRESULT hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&desc,&swap_,&device_,&level,&context_);
        if(FAILED(hr))check(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&desc,&swap_,&device_,&level,&context_),"Create D3D11 device");
        resize();
        auto compile=[&](const char* entry,const char* target) {
            ComPtr<ID3DBlob> bytecode,errors;
            HRESULT result=D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,entry,target,D3DCOMPILE_ENABLE_STRICTNESS,0,&bytecode,&errors);
            if(FAILED(result))throw std::runtime_error(errors?static_cast<const char*>(errors->GetBufferPointer()):"Shader compilation failed");
            return bytecode;
        };
        auto vsCode=compile("VSMain","vs_5_0"),psCode=compile("PSMain","ps_5_0");
        check(device_->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs_),"Create VS");
        check(device_->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps_),"Create PS");
        auto bvs=compile("VSBillboard","vs_5_0"),bps=compile("PSBillboard","ps_5_0");
        check(device_->CreateVertexShader(bvs->GetBufferPointer(),bvs->GetBufferSize(),nullptr,&billboardVs_),"Create billboard VS");
        check(device_->CreatePixelShader(bps->GetBufferPointer(),bps->GetBufferSize(),nullptr,&billboardPs_),"Create billboard PS");
        const D3D11_INPUT_ELEMENT_DESC elements[]={
            {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,24,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"INSTANCEPOS",0,DXGI_FORMAT_R32G32B32A32_FLOAT,1,0,D3D11_INPUT_PER_INSTANCE_DATA,1},
            {"INSTANCECOLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,1,16,D3D11_INPUT_PER_INSTANCE_DATA,1}};
        check(device_->CreateInputLayout(elements,5,vsCode->GetBufferPointer(),vsCode->GetBufferSize(),&layout_),"Create input layout");
        D3D11_SAMPLER_DESC s{};s.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;s.AddressU=D3D11_TEXTURE_ADDRESS_WRAP;s.AddressV=D3D11_TEXTURE_ADDRESS_WRAP;s.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;s.MaxLOD=D3D11_FLOAT32_MAX;
        check(device_->CreateSamplerState(&s,&sampler_),"Create sampler");
        D3D11_RASTERIZER_DESC r{};r.FillMode=D3D11_FILL_SOLID;r.CullMode=D3D11_CULL_NONE;r.DepthClipEnable=TRUE;
        check(device_->CreateRasterizerState(&r,&raster_),"Create rasterizer");
        D3D11_BUFFER_DESC cb{};cb.ByteWidth=sizeof(CameraBuffer);cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        check(device_->CreateBuffer(&cb,nullptr,&cameraBuffer_),"Create camera buffer");
        D3D11_BUFFER_DESC ib{};ib.ByteWidth=sizeof(Instance)*3000;ib.Usage=D3D11_USAGE_DYNAMIC;ib.BindFlags=D3D11_BIND_VERTEX_BUFFER;ib.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        check(device_->CreateBuffer(&ib,nullptr,&instanceBuffer_),"Create instance buffer");
        const uint8_t white[4]={255,255,255,255};whiteTexture_=makeTexture(white,1,1);
    }
    void makeCube() {
        auto face=[&](XMFLOAT3 a,XMFLOAT3 b,XMFLOAT3 c,XMFLOAT3 d,XMFLOAT3 n) {
            auto push=[&](XMFLOAT3 p,float u,float v){cube_.push_back({p.x,p.y,p.z,n.x,n.y,n.z,u,v});};
            push(a,0,0);push(b,1,0);push(c,1,1);push(a,0,0);push(c,1,1);push(d,0,1);
        };
        face({-.5f,-.5f,-.5f},{.5f,-.5f,-.5f},{.5f,.5f,-.5f},{-.5f,.5f,-.5f},{0,0,-1});
        face({.5f,-.5f,.5f},{-.5f,-.5f,.5f},{-.5f,.5f,.5f},{.5f,.5f,.5f},{0,0,1});
        face({-.5f,-.5f,.5f},{-.5f,-.5f,-.5f},{-.5f,.5f,-.5f},{-.5f,.5f,.5f},{-1,0,0});
        face({.5f,-.5f,-.5f},{.5f,-.5f,.5f},{.5f,.5f,.5f},{.5f,.5f,-.5f},{1,0,0});
        face({-.5f,.5f,-.5f},{.5f,.5f,-.5f},{.5f,.5f,.5f},{-.5f,.5f,.5f},{0,1,0});
        face({-.5f,-.5f,.5f},{.5f,-.5f,.5f},{.5f,-.5f,-.5f},{-.5f,-.5f,-.5f},{0,-1,0});
        cubeBuffer_=makeImmutableVertexBuffer(cube_);
    }
    void buildObjects() {
        std::mt19937 rng(2026);
        std::uniform_real_distribution<float> jitter(-1.3f,1.3f),scale(0.65f,1.4f),color(0.45f,1.f);
        for(int z=-25;z<25;++z)for(int x=-25;x<25;++x) {
            const float px=x*5.f+jitter(rng),pz=z*5.f+jitter(rng);
            if(std::abs(px)<23 && std::abs(pz)<17)continue;
            const float size=scale(rng),height=0.7f+size*0.5f;
            objects_.push_back({px,height,pz,size,color(rng),color(rng),color(rng),1});
            const float h=size*1.3f; // covers cube and rotating billboard
            boxes_.push_back({{px-h,height-h,pz-h},{px+h,height+h,pz+h}});
        }
        tree_.build(boxes_);
    }
    void loadScene() {
        const fs::path assets=findAssets();
        std::unordered_map<std::string,int> ids;
        materials_=loadMaterials(assets/L"sponza.mtl",ids);
        model_=loadObj(assets/L"sponza.obj",ids);
        modelBuffer_=makeImmutableVertexBuffer(model_.vertices);
        for(auto& mat:materials_) {
            uint32_t w=0,h=0;
            auto pixels=readTga(mat.diffuse,w,h);
            mat.texture=pixels.empty()?whiteTexture_:makeTexture(pixels.data(),w,h);
        }
        makeCube();buildObjects();
        modelCenter_={(model_.bounds.min.x+model_.bounds.max.x)*0.5f,(model_.bounds.min.y+model_.bounds.max.y)*0.5f,(model_.bounds.min.z+model_.bounds.max.z)*0.5f};
        const float dx=model_.bounds.max.x-model_.bounds.min.x,dy=model_.bounds.max.y-model_.bounds.min.y,dz=model_.bounds.max.z-model_.bounds.min.z;
        modelRadius_=0.51f*std::sqrt(dx*dx+dy*dy+dz*dz);
        std::vector<Vertex> quad{
            {-.5f,.5f,0,0,0,-1,0,0},{.5f,.5f,0,0,0,-1,1,0},{.5f,-.5f,0,0,0,-1,1,1},
            {-.5f,.5f,0,0,0,-1,0,0},{.5f,-.5f,0,0,0,-1,1,1},{-.5f,-.5f,0,0,0,-1,0,1}};
        quadBuffer_=makeImmutableVertexBuffer(quad);
        uint32_t lionWidth=0,lionHeight=0;
        auto lionPixels=readTga(assets/L"textures"/L"lion.tga",lionWidth,lionHeight);
        if(lionPixels.empty())throw std::runtime_error("Cannot load textures/lion.tga for billboards");
        lionTexture_=makeTexture(lionPixels.data(),lionWidth,lionHeight);
        resize();
    }
    void drawBillboards(const std::vector<Instance>& instances,ID3D11ShaderResourceView* texture) {
        if(instances.empty())return;
        uploadInstances(instances);
        context_->VSSetShader(billboardVs_.Get(),nullptr,0);context_->PSSetShader(billboardPs_.Get(),nullptr,0);
        ID3D11Buffer* buffers[]={quadBuffer_.Get(),instanceBuffer_.Get()};UINT strides[]={sizeof(Vertex),sizeof(Instance)},offsets[]={0,0};
        context_->IASetVertexBuffers(0,2,buffers,strides,offsets);context_->PSSetShaderResources(0,1,&texture);
        context_->DrawInstanced(6,static_cast<UINT>(instances.size()),0,0);
    }
    void updateCamera(float dt) {
        const float turn=1.7f*dt;
        if(keys_[VK_LEFT])yaw_-=turn;if(keys_[VK_RIGHT])yaw_+=turn;
        if(keys_[VK_UP])pitch_+=turn;if(keys_[VK_DOWN])pitch_-=turn;
        if(mouseHeld_) {
            POINT p;GetCursorPos(&p);
            yaw_+=(p.x-lastMouse_.x)*0.003f;pitch_-=(p.y-lastMouse_.y)*0.003f;
            lastMouse_=p;
        }
        pitch_=std::clamp(pitch_,-1.48f,1.48f);
        const float speed=(keys_[VK_SHIFT]?22.f:9.f)*dt;
        const Vec3 forward{std::sin(yaw_),0,std::cos(yaw_)};
        const Vec3 right{std::cos(yaw_),0,-std::sin(yaw_)};
        if(keys_['W']){camera_.x+=forward.x*speed;camera_.z+=forward.z*speed;}
        if(keys_['S']){camera_.x-=forward.x*speed;camera_.z-=forward.z*speed;}
        if(keys_['D']){camera_.x+=right.x*speed;camera_.z+=right.z*speed;}
        if(keys_['A']){camera_.x-=right.x*speed;camera_.z-=right.z*speed;}
        if(keys_['E'])camera_.y+=speed;if(keys_['Q'])camera_.y-=speed;
    }
    void selectVisible(const Frustum& frustum) {
        visible_.clear();stats_={};
        if(!culling_) {for(uint32_t i=0;i<objects_.size();++i)visible_.push_back(i);return;}
        if(octree_) {tree_.visible(frustum,visible_,stats_);return;}
        for(uint32_t i=0;i<boxes_.size();++i) {
            ++stats_.objectsTested;
            if(frustum.classify(boxes_[i])!=Relation::Outside)visible_.push_back(i);
        }
    }
    void uploadInstances(const std::vector<Instance>& instances) {
        D3D11_MAPPED_SUBRESOURCE mapped{};check(context_->Map(instanceBuffer_.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"Map instance buffer");
        memcpy(mapped.pData,instances.data(),instances.size()*sizeof(Instance));context_->Unmap(instanceBuffer_.Get(),0);
    }
    void draw(float dt) {
        if(!width_ || !height_)return;
        // WM_SIZE updates dimensions before the swap-chain resources are resized.
        D3D11_TEXTURE2D_DESC old{};depthTexture_->GetDesc(&old);
        if(old.Width!=width_ || old.Height!=height_)resize();
        updateCamera(dt);
        const XMVECTOR eye=XMVectorSet(camera_.x,camera_.y,camera_.z,1);
        const XMVECTOR direction=XMVectorSet(std::cos(pitch_)*std::sin(yaw_),std::sin(pitch_),std::cos(pitch_)*std::cos(yaw_),0);
        const XMMATRIX view=XMMatrixLookToLH(eye,direction,XMVectorSet(0,1,0,0));
        const XMMATRIX projection=XMMatrixPerspectiveFovLH(XMConvertToRadians(70.f),float(width_)/height_,0.15f,250.f);
        const XMMATRIX vp=XMMatrixMultiply(view,projection);
        XMFLOAT4X4 raw;XMStoreFloat4x4(&raw,vp);
        float matrix[4][4];memcpy(matrix,&raw,sizeof(matrix));
        const Frustum frustum=Frustum::fromMatrix(matrix);
        selectVisible(frustum);
        CameraBuffer camera{};XMStoreFloat4x4(&camera.viewProj,XMMatrixTranspose(vp));camera.lightDirection={-0.4f,-1.f,-0.3f,0};
        camera.cameraRight={std::cos(yaw_),0,-std::sin(yaw_),0};
        camera.cameraUp={-std::sin(pitch_)*std::sin(yaw_),std::cos(pitch_),-std::sin(pitch_)*std::cos(yaw_),0};
        context_->UpdateSubresource(cameraBuffer_.Get(),0,nullptr,&camera,0,0);
        const float clear[4]={0.07f,0.10f,0.16f,1};
        context_->ClearRenderTargetView(target_.Get(),clear);
        context_->ClearDepthStencilView(depthView_.Get(),D3D11_CLEAR_DEPTH,1,0);
        ID3D11RenderTargetView* rtv=target_.Get();context_->OMSetRenderTargets(1,&rtv,depthView_.Get());
        context_->RSSetState(raster_.Get());context_->IASetInputLayout(layout_.Get());context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vs_.Get(),nullptr,0);context_->PSSetShader(ps_.Get(),nullptr,0);
        ID3D11Buffer* cb=cameraBuffer_.Get();context_->VSSetConstantBuffers(0,1,&cb);context_->PSSetConstantBuffers(0,1,&cb);
        ID3D11SamplerState* sampler=sampler_.Get();context_->PSSetSamplers(0,1,&sampler);
        const Lod modelLod=selectLod(camera_,modelCenter_,60.f,150.f,forcedLod_);
        const float r=modelRadius_*1.415f;
        const Aabb billboardBounds{{modelCenter_.x-r,modelCenter_.y-r,modelCenter_.z-r},{modelCenter_.x+r,modelCenter_.y+r,modelCenter_.z+r}};
        const bool modelVisible=!culling_ || frustum.classify(modelLod==Lod::Billboard?billboardBounds:model_.bounds)!=Relation::Outside;
        if(modelVisible && modelLod==Lod::Model) {
            uploadInstances({Instance{0,0,0,1,1,1,1,1}});
            ID3D11Buffer* buffers[]={modelBuffer_.Get(),instanceBuffer_.Get()};UINT strides[]={sizeof(Vertex),sizeof(Instance)},offsets[]={0,0};
            context_->IASetVertexBuffers(0,2,buffers,strides,offsets);
            for(const Batch& batch:model_.batches) {
                ID3D11ShaderResourceView* texture=materials_[batch.material].texture.Get();
                context_->PSSetShaderResources(0,1,&texture);
                context_->DrawInstanced(batch.count,1,batch.first,0);
            }
        }
        std::vector<Instance> nearInstances,billboardInstances;
        size_t hiddenCount=0;
        for(uint32_t id:visible_) {
            const auto& o=objects_[id];
            switch(selectLod(camera_,{o.x,o.y,o.z},30.f,85.f,forcedLod_)) {
                case Lod::Model:nearInstances.push_back(o);break;
                case Lod::Billboard:{auto b=o;b.scale*=1.8f;b.r=b.g=b.b=b.a=1.f;billboardInstances.push_back(b);break;}
                case Lod::Hidden:++hiddenCount;break;
            }
        }
        if(!nearInstances.empty()) {
            const auto& instances=nearInstances;
            uploadInstances(instances);
            ID3D11Buffer* buffers[]={cubeBuffer_.Get(),instanceBuffer_.Get()};UINT strides[]={sizeof(Vertex),sizeof(Instance)},offsets[]={0,0};
            context_->IASetVertexBuffers(0,2,buffers,strides,offsets);
            ID3D11ShaderResourceView* white=whiteTexture_.Get();context_->PSSetShaderResources(0,1,&white);
            context_->DrawInstanced(static_cast<UINT>(cube_.size()),static_cast<UINT>(instances.size()),0,0);
        }
        drawBillboards(billboardInstances,lionTexture_.Get());
        if(modelVisible && modelLod==Lod::Billboard)
            drawBillboards({Instance{modelCenter_.x,modelCenter_.y,modelCenter_.z,modelRadius_*2,1,1,1,1}},lionTexture_.Get());
        if(selfTest_)verifyFrame();
        check(swap_->Present(1,0),"Present");
        ++frames_;
        const auto now=std::chrono::steady_clock::now();
        const float elapsed=std::chrono::duration<float>(now-fpsStart_).count();
        if(elapsed>=0.5f) {
            fps_=frames_/elapsed;frames_=0;fpsStart_=now;
            std::wostringstream title;
            title<<L"CG Homework 4 | "<<(culling_?(octree_?L"Frustum + Octree":L"Frustum only"):L"No culling")
                 <<L" | LOD "<<(forcedLod_<0?L"Auto":forcedLod_==0?L"0":forcedLod_==1?L"1":L"2")
                 <<L" | cubes 0/1/2: "<<nearInstances.size()<<L"/"<<billboardInstances.size()<<L"/"<<hiddenCount
                 <<L" | Sponza: "<<static_cast<int>(modelLod)
                 <<L" | AABB tests "<<stats_.objectsTested<<L" | nodes "<<stats_.nodesTested
                 <<L" | FPS "<<std::fixed<<std::setprecision(1)<<fps_
                 <<L" | F1/F2 culling, F3 LOD, Home overview";
            SetWindowTextW(hwnd_,title.str().c_str());
        }
    }
public:
    int run(HINSTANCE instance) {
        selfTest_=std::wstring(GetCommandLineW()).find(L"--self-test")!=std::wstring::npos;
        WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.lpfnWndProc=windowProc;wc.hInstance=instance;wc.lpszClassName=L"CGHomework4Window";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.style=CS_HREDRAW|CS_VREDRAW;
        if(!RegisterClassExW(&wc))throw std::runtime_error("RegisterClassEx failed");
        hwnd_=CreateWindowExW(0,wc.lpszClassName,L"CG Homework 4",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,1280,720,nullptr,nullptr,instance,this);
        if(!hwnd_)throw std::runtime_error("CreateWindowEx failed");
        if(!selfTest_){ShowWindow(hwnd_,SW_SHOW);UpdateWindow(hwnd_);}
        initGraphics();loadScene();
        if(selfTest_) {
            std::ofstream("lod_selftest.txt")<<"D3D11 render verification\n";
            camera_={0,18,-45};yaw_=0;pitch_=-0.15f;
            for(int mode=-1;mode<=2;++mode){forcedLod_=mode;draw(0);}
            std::ofstream("lod_selftest.txt",std::ios::app)<<"PASS\n";
            return 0;
        }
        auto previous=std::chrono::steady_clock::now();
        MSG msg{};
        while(true) {
            while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
                if(msg.message==WM_QUIT)return static_cast<int>(msg.wParam);
                TranslateMessage(&msg);DispatchMessageW(&msg);
            }
            const auto now=std::chrono::steady_clock::now();
            const float dt=std::min(0.05f,std::chrono::duration<float>(now-previous).count());previous=now;
            draw(dt);
        }
    }
};

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int) {
    try {App app;return app.run(instance);}
    catch(const std::exception& e) {
        if(std::wstring(GetCommandLineW()).find(L"--self-test")!=std::wstring::npos)std::ofstream("lod_selftest.txt",std::ios::app)<<"FAIL: "<<e.what()<<"\n";
        else MessageBoxA(nullptr,e.what(),"CG Homework 4 error",MB_ICONERROR);
        return 1;
    }
}



