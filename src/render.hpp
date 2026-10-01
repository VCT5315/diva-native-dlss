#pragma once
#include "common.hpp"
#include "state.hpp"
#include <cstdint>
#include <cstring>
#include <cmath>
#include <atomic>

inline ComPtr<ID3DBlob> Compile(const char* text,const char* entry,const char* profile) {
    ComPtr<ID3DBlob> code,error;
    HRESULT hr=D3DCompile(text,strlen(text),"DivaNativeDLSS",nullptr,nullptr,entry,profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
    if(FAILED(hr)){Log("shader %s failed=%08X %s",entry,hr,error?static_cast<char*>(error->GetBufferPointer()):"");code.Reset();}return code;
}
struct Surface {
    ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;ComPtr<ID3D11UnorderedAccessView> uav;
    bool Create(ID3D11Device* d,UINT w,UINT h,DXGI_FORMAT f,UINT bind) {
        D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=1;desc.ArraySize=1;desc.Format=f;desc.SampleDesc.Count=1;desc.BindFlags=bind;
        HRESULT hr=d->CreateTexture2D(&desc,nullptr,&texture);
        if(SUCCEEDED(hr)&&(bind&D3D11_BIND_SHADER_RESOURCE))hr=d->CreateShaderResourceView(texture.Get(),nullptr,&srv);
        if(SUCCEEDED(hr)&&(bind&D3D11_BIND_RENDER_TARGET))hr=d->CreateRenderTargetView(texture.Get(),nullptr,&rtv);
        if(SUCCEEDED(hr)&&(bind&D3D11_BIND_UNORDERED_ACCESS))hr=d->CreateUnorderedAccessView(texture.Get(),nullptr,&uav);
        if(FAILED(hr)){Log("surface failed=%08X extent=%ux%u format=%u",hr,w,h,f);return false;}return true;
    }
};
struct DrawKey {
    uintptr_t vs=0,vb=0,ib=0,texture=0;
    UINT count=0,start=0,stride=0,offset=0,occurrence=0;
    UINT indexOffset=0,indexFormat=0,topology=0;
    INT base=0;
    bool operator==(const DrawKey& b) const {return vs==b.vs&&vb==b.vb&&ib==b.ib&&texture==b.texture&&count==b.count&&start==b.start&&stride==b.stride&&offset==b.offset&&occurrence==b.occurrence&&base==b.base&&indexOffset==b.indexOffset&&indexFormat==b.indexFormat&&topology==b.topology;}
};
struct KeyHash { size_t operator()(const DrawKey& k) const {
    size_t h=1469598103934665603ull;
    for(auto v:{k.vs,k.vb,k.ib,k.texture,uintptr_t(k.count),uintptr_t(k.start),uintptr_t(k.stride),uintptr_t(k.offset),uintptr_t(k.occurrence),uintptr_t(k.base),uintptr_t(k.indexOffset),uintptr_t(k.indexFormat),uintptr_t(k.topology)}){h^=v;h*=1099511628211ull;}return h;
} };
struct GeometryHistory {
    ComPtr<ID3D11Buffer> buffers[2];ComPtr<ID3D11ShaderResourceView> srvs[2];
    uint64_t frame=UINT64_MAX;UINT write=0;size_t bytes=0;
};
using DrawCall=void(*)(ID3D11DeviceContext*,bool,UINT,UINT,INT);
class Renderer {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11GeometryShader> motionGS;ComPtr<ID3D11PixelShader> motionPS,copyPS,composePS,debugPS,heldPS,badgePS;
    ComPtr<ID3D11GeometryShader> captureStream;
    ComPtr<ID3D11VertexShader> fullscreenVS;
    ComPtr<ID3D11DepthStencilState> equalDepth,noDepth;
    ComPtr<ID3D11BlendState> noBlend;
    ComPtr<ID3D11RasterizerState> fullscreenRaster;
    ComPtr<ID3D11Buffer> constants;
    Surface motion,mask,input,output,backCopy,pausedRaw;
    ComPtr<ID3D11Texture2D> depthCopy,sceneDepth;
    ComPtr<ID3D11ShaderResourceView> depthSRV;
    std::unordered_map<DrawKey,GeometryHistory,KeyHash> history;
    std::unordered_map<DrawKey,UINT,KeyHash> occurrences;
    size_t historyBytes=0;size_t historyLimit=128*1024*1024;
    UINT width=0,height=0,screenWidth=0,screenHeight=0;bool initialized=false,failed=false,clear=true,reset=true;
    uint64_t frame=0;
    UINT nativeDraws=0,historyDraws=0,uiDraws=0,overlayDepth=0;
    bool resolvedBeforeUI=false,framePublished=false,frameHeld=false;
    uint64_t dlssFrames=0;
    UINT totalDraws=0,jitteredDraws=0;unsigned int viewportDiagnostics=0;bool traceOnly=false,captureOnly=false;
    int configuredFlags=-1;
    float jitterX=0,jitterY=0;bool cachedDisplayValid=false;
    Surface cachedDisplay;
    Surface badge;ComPtr<ID3D11BlendState> badgeBlend;
    uint64_t toastUntil=0;bool badgeDirty=true,showStatus=true;
    std::atomic<UINT> pendingKeys{0};bool windowKeyEvents=false;bool previousKeys[3]{};
    Dlss dlss;
    static constexpr const char* motionCode=R"(
Buffer<float4> Previous : register(t0);
cbuffer Frame : register(b0) {float2 Extent;float2 Jitter;};
struct V {float4 p:SV_Position;};
struct G {float4 p:SV_Position;float4 old:TEXCOORD0;};
[maxvertexcount(3)]void CaptureGS(triangle V v[3],inout TriangleStream<V> stream){for(uint i=0;i<3;i++)stream.Append(v[i]);}
[maxvertexcount(3)]void MotionGS(triangle V v[3],uint primitive:SV_PrimitiveID,inout TriangleStream<G> stream){
 for(uint i=0;i<3;i++){G o;o.p=v[i].p;o.old=Previous[primitive*3+i];stream.Append(o);}
}
float2 MotionPS(G v):SV_Target {
 if(v.old.w<=0.000001)return 0;
 float2 oldUV=v.old.xy/v.old.w*float2(0.5,-0.5)+0.5;
 float2 currentUV=v.p.xy/Extent;
 // History stores the game's unjittered clip positions. Remove this frame's
 // rasterization offset so that the motion vectors contain object motion only.
 float2 delta=(oldUV-currentUV)*Extent+Jitter;
 return all(abs(delta)<Extent*0.5)?delta:0;
}
)";
    static constexpr const char* fullscreenCode=R"(
Texture2D<float4> Source:register(t0);Texture2D<float4> Neural:register(t1);
Texture2D<float> Mask:register(t2);Texture2D<float2> Motion:register(t3);
Texture2D<float4> PreviousSource:register(t4);
struct V{float4 p:SV_Position;};
V FullVS(uint id:SV_VertexID){V v;v.p=float4(id==2?3:-1,id==1?3:-1,0,1);return v;}
float4 CopyPS(V v):SV_Target{return Source.Load(int3(v.p.xy,0));}
float4 ComposePS(V v):SV_Target {int3 p=int3(v.p.xy,0);return lerp(Source.Load(p),Neural.Load(p),Mask.Load(p));}
float4 DebugPS(V v):SV_Target {float2 mv=Motion.Load(int3(v.p.xy,0));return float4(0.5+mv/64,0.5,1);}
float4 BadgePS(V v):SV_Target {float coverage=Source.Load(int3(int2(v.p.xy)-int2(20,20),0)).r;return float4(lerp(float3(.035,.045,.06),float3(.88,.95,1),coverage),lerp(.85,1,coverage));}
float4 HeldPS(V v):SV_Target {
 int2 p=int2(v.p.xy);float4 current=Source.Load(int3(p,0));
 // The paused game redraws its cached scene and may add new menus. Reuse the
 // processed pixels only where the raw scene and its neighbours still match.
 float difference=0;int2 offsets[5]={int2(0,0),int2(3,0),int2(-3,0),int2(0,3),int2(0,-3)};
 for(uint i=0;i<5;i++){float3 d=abs(Source.Load(int3(p+offsets[i],0)).rgb-PreviousSource.Load(int3(p+offsets[i],0)).rgb);difference=max(difference,max(d.x,max(d.y,d.z)));}
 return difference<=1.5/255.0&&Mask.Load(int3(p,0))>0.5?Neural.Load(int3(p,0)):current;
}
)";
    void BindFullscreen(ID3D11RenderTargetView* target,ID3D11PixelShader* ps,UINT w,UINT h) {
        context->OMSetRenderTargets(1,&target,nullptr);context->OMSetDepthStencilState(noDepth.Get(),0);float blend[4]{};
        context->OMSetBlendState(noBlend.Get(),blend,0xffffffff);context->RSSetState(fullscreenRaster.Get());
        D3D11_VIEWPORT vp{0,0,float(w),float(h),0,1};context->RSSetViewports(1,&vp);
        context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(fullscreenVS.Get(),nullptr,0);context->GSSetShader(nullptr,nullptr,0);context->HSSetShader(nullptr,nullptr,0);context->DSSetShader(nullptr,nullptr,0);context->PSSetShader(ps,nullptr,0);
    }
    bool EnsureSurfaces(UINT w,UINT h) {
        if(width==w&&height==h&&motion.texture)return true;
        Resize();width=w;height=h;
        UINT rt=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        if(!motion.Create(device.Get(),w,h,DXGI_FORMAT_R16G16_FLOAT,rt)||!mask.Create(device.Get(),w,h,DXGI_FORMAT_R8_UNORM,rt)||!input.Create(device.Get(),w,h,DXGI_FORMAT_R8G8B8A8_UNORM,rt)||!output.Create(device.Get(),w,h,DXGI_FORMAT_R8G8B8A8_UNORM,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS))return false;
        UpdateFrameConstants();
        Log("render surfaces extent=%ux%u history_limit_mb=%zu",w,h,historyLimit/1024/1024);return true;
    }
    bool CopyDepth() {
        if(!sceneDepth)return false;
        D3D11_TEXTURE2D_DESC desc{};sceneDepth->GetDesc(&desc);
        if(desc.Width!=width||desc.Height!=height||desc.SampleDesc.Count!=1)return false;
        DXGI_FORMAT viewFormat;
        switch(desc.Format){
        case DXGI_FORMAT_R32_TYPELESS:case DXGI_FORMAT_D32_FLOAT:desc.Format=DXGI_FORMAT_R32_TYPELESS;viewFormat=DXGI_FORMAT_R32_FLOAT;break;
        case DXGI_FORMAT_R24G8_TYPELESS:case DXGI_FORMAT_D24_UNORM_S8_UINT:desc.Format=DXGI_FORMAT_R24G8_TYPELESS;viewFormat=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;break;
        case DXGI_FORMAT_R16_TYPELESS:case DXGI_FORMAT_D16_UNORM:desc.Format=DXGI_FORMAT_R16_TYPELESS;viewFormat=DXGI_FORMAT_R16_UNORM;break;
        default:return false;
        }
        if(!depthCopy){desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.Usage=D3D11_USAGE_DEFAULT;desc.CPUAccessFlags=0;desc.MiscFlags=0;
            HRESULT hr=device->CreateTexture2D(&desc,nullptr,&depthCopy);if(FAILED(hr))return false;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=viewFormat;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=desc.MipLevels;
            hr=device->CreateShaderResourceView(depthCopy.Get(),&sd,&depthSRV);if(FAILED(hr)){depthCopy.Reset();return false;}
        }
        context->CopyResource(depthCopy.Get(),sceneDepth.Get());return true;
    }
    static float Halton(UINT index,UINT base){float value=0,fraction=1;while(index){fraction/=float(base);value+=fraction*float(index%base);index/=base;}return value;}
    void UpdateFrameConstants(){float values[4]={float(width),float(height),jitterX,jitterY};context->UpdateSubresource(constants.Get(),0,nullptr,values,0,0);}
    void ReadSettings(){
        const auto ini=ModDirectory()+L"\\settings.ini";
        showStatus=GetPrivateProfileIntW(L"DLSS",L"ShowStatus",1,ini.c_str())!=0;
        int flags=(GetPrivateProfileIntW(L"DLSS",L"Enable",1,ini.c_str())?1:0)|(GetPrivateProfileIntW(L"DLSS",L"DebugMotion",0,ini.c_str())?2:0)|(GetPrivateProfileIntW(L"DLSS",L"TraceOnly",0,ini.c_str())?4:0)|(GetPrivateProfileIntW(L"DLSS",L"CaptureOnly",0,ini.c_str())?8:0);
        if(flags==configuredFlags)return;
        configuredFlags=flags;enabled=(flags&1)!=0;debug=(flags&2)!=0;traceOnly=(flags&4)!=0;captureOnly=(flags&8)!=0;
        Resize();ShowToast();Log("settings enable=%d debug=%d trace=%d capture=%d",enabled,debug,traceOnly,captureOnly);
    }
    void ShowToast(){toastUntil=GetTickCount64()+3500;badgeDirty=true;}
    void PersistSetting(const wchar_t* key,bool value){const auto ini=ModDirectory()+L"\\settings.ini";WritePrivateProfileStringW(L"DLSS",key,value?L"1":L"0",ini.c_str());}
    void RenderBadge(IDXGISwapChain* swap,DrawCall call){
        if(!showStatus||GetTickCount64()>toastUntil)return;
        PipelineState original(context.Get());
        if(badgeDirty){
            const wchar_t* message=debug?L"运动矢量预览   F5 返回":enabled&&!traceOnly&&!captureOnly?L"DLSS / DLAA 已开启   F4 切换":L"DLSS / DLAA 已关闭   F4 切换";
            HDC dc=CreateCompatibleDC(nullptr);if(!dc)return;
            HFONT font=CreateFontW(-18,0,0,0,FW_MEDIUM,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
            auto oldFont=SelectObject(dc,font);SIZE size{};GetTextExtentPoint32W(dc,message,int(wcslen(message)),&size);
            UINT w=UINT(size.cx)+28,h=40;BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=LONG(w);info.bmiHeader.biHeight=-LONG(h);info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
            void* pixels=nullptr;HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
            if(bitmap&&pixels){auto oldBitmap=SelectObject(dc,bitmap);memset(pixels,0,size_t(w)*h*4);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(255,255,255));TextOutW(dc,14,(int(h)-size.cy)/2,message,int(wcslen(message)));GdiFlush();
                badge={};if(badge.Create(device.Get(),w,h,DXGI_FORMAT_R8G8B8A8_UNORM,D3D11_BIND_SHADER_RESOURCE)){context->UpdateSubresource(badge.texture.Get(),0,nullptr,pixels,w*4,0);badgeDirty=false;}
                SelectObject(dc,oldBitmap);DeleteObject(bitmap);
            }
            SelectObject(dc,oldFont);DeleteObject(font);DeleteDC(dc);
        }
        if(!badge.srv)return;
        ComPtr<ID3D11Texture2D> back;swap->GetBuffer(0,IID_PPV_ARGS(&back));if(!back)return;
        ComPtr<ID3D11RenderTargetView> rt;device->CreateRenderTargetView(back.Get(),nullptr,&rt);if(!rt)return;
        D3D11_TEXTURE2D_DESC td{};badge.texture->GetDesc(&td);BindFullscreen(rt.Get(),badgePS.Get(),td.Width,td.Height);
        D3D11_VIEWPORT vp{20,20,float(td.Width),float(td.Height),0,1};context->RSSetViewports(1,&vp);float factor[4]{};context->OMSetBlendState(badgeBlend.Get(),factor,0xffffffff);
        auto text=badge.srv.Get();context->PSSetShaderResources(0,1,&text);call(context.Get(),false,3,0,0);
    }
public:
    void UseWindowKeyEvents(bool value){windowKeyEvents=value;}
    void QueueKey(UINT key){if(key>=VK_F4&&key<=VK_F6)pendingKeys.fetch_or(1u<<(key-VK_F4));}
    struct ViewportRestore {
        ID3D11DeviceContext* c=nullptr;D3D11_VIEWPORT viewport{};
        ~ViewportRestore(){if(c)c->RSSetViewports(1,&viewport);}
    };
    void BeforeDraw(ID3D11DeviceContext* c,ViewportRestore& restore){
        if(c!=context.Get()||!initialized||failed||!enabled||traceOnly||captureOnly||overlayDepth||resolvedBeforeUI)return;
        UINT n=1;D3D11_VIEWPORT vp{};c->RSGetViewports(&n,&vp);
        if(n!=1||vp.TopLeftX!=0||vp.TopLeftY!=0||UINT(vp.Width)!=screenWidth||UINT(vp.Height)!=screenHeight||vp.Width<640||vp.Height<360)return;
        ComPtr<ID3D11DepthStencilView> depth;c->OMGetRenderTargets(0,nullptr,&depth);if(!depth)return;
        ComPtr<ID3D11DepthStencilState> state;c->OMGetDepthStencilState(&state,nullptr);D3D11_DEPTH_STENCIL_DESC sd{};if(state){state->GetDesc(&sd);if(!sd.DepthEnable)return;}
        ComPtr<ID3D11Resource> resource;depth->GetResource(&resource);ComPtr<ID3D11Texture2D> texture;resource.As(&texture);if(!texture)return;
        D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);if(td.Width!=screenWidth||td.Height!=screenHeight||td.SampleDesc.Count!=1)return;
        // D3D11 accepts fractional viewport origins, so this adds real subpixel
        // samples without requiring a TAA implementation or editing game shaders.
        UINT phase=UINT(frame%8)+1;jitterX=Halton(phase,2)-.5f;jitterY=Halton(phase,3)-.5f;
        restore.c=c;restore.viewport=vp;vp.TopLeftX+=jitterX;vp.TopLeftY+=jitterY;c->RSSetViewports(1,&vp);++jitteredDraws;
        if(width&&height)UpdateFrameConstants();
    }
    bool enabled=true,debug=false;
    void SetScreenExtent(UINT w,UINT h){screenWidth=w;screenHeight=h;}
    void Initialize(ID3D11Device* d,ID3D11DeviceContext* c) {
        if(initialized)return;device=d;context=c;
        const auto ini=ModDirectory()+L"\\settings.ini";
        ReadSettings();
        historyLimit=size_t(std::clamp(GetPrivateProfileIntW(L"DLSS",L"HistoryMB",128,ini.c_str()),32u,512u))*1024*1024;
        auto gs=Compile(motionCode,"MotionGS","gs_5_0"),ps=Compile(motionCode,"MotionPS","ps_5_0"),capture=Compile(motionCode,"CaptureGS","gs_5_0");
        auto vs=Compile(fullscreenCode,"FullVS","vs_5_0"),cp=Compile(fullscreenCode,"CopyPS","ps_5_0"),np=Compile(fullscreenCode,"ComposePS","ps_5_0"),dp=Compile(fullscreenCode,"DebugPS","ps_5_0"),hp=Compile(fullscreenCode,"HeldPS","ps_5_0"),bp=Compile(fullscreenCode,"BadgePS","ps_5_0");
        if(!gs||!ps||!vs||!cp||!np||!dp||!capture||!hp||!bp){failed=true;return;}
        HRESULT hr=d->CreateGeometryShader(gs->GetBufferPointer(),gs->GetBufferSize(),nullptr,&motionGS);
        D3D11_SO_DECLARATION_ENTRY streamEntry{0,"SV_Position",0,0,4,0};UINT streamStride=16;
        if(SUCCEEDED(hr))hr=d->CreateGeometryShaderWithStreamOutput(capture->GetBufferPointer(),capture->GetBufferSize(),&streamEntry,1,&streamStride,1,D3D11_SO_NO_RASTERIZED_STREAM,nullptr,&captureStream);
        if(SUCCEEDED(hr))hr=d->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&motionPS);
        if(SUCCEEDED(hr))hr=d->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&fullscreenVS);
        if(SUCCEEDED(hr))hr=d->CreatePixelShader(cp->GetBufferPointer(),cp->GetBufferSize(),nullptr,&copyPS);
        if(SUCCEEDED(hr))hr=d->CreatePixelShader(np->GetBufferPointer(),np->GetBufferSize(),nullptr,&composePS);
        if(SUCCEEDED(hr))hr=d->CreatePixelShader(dp->GetBufferPointer(),dp->GetBufferSize(),nullptr,&debugPS);
        if(SUCCEEDED(hr))hr=d->CreatePixelShader(hp->GetBufferPointer(),hp->GetBufferSize(),nullptr,&heldPS);
        if(SUCCEEDED(hr))hr=d->CreatePixelShader(bp->GetBufferPointer(),bp->GetBufferSize(),nullptr,&badgePS);
        D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=TRUE;ds.DepthFunc=D3D11_COMPARISON_EQUAL;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
        if(SUCCEEDED(hr))hr=d->CreateDepthStencilState(&ds,&equalDepth);ds.DepthEnable=FALSE;if(SUCCEEDED(hr))hr=d->CreateDepthStencilState(&ds,&noDepth);
        D3D11_BLEND_DESC bd{};bd.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        if(SUCCEEDED(hr))hr=d->CreateBlendState(&bd,&noBlend);
        bd.RenderTarget[0].BlendEnable=TRUE;bd.RenderTarget[0].BlendOp=bd.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlend=D3D11_BLEND_SRC_ALPHA;bd.RenderTarget[0].DestBlend=D3D11_BLEND_INV_SRC_ALPHA;bd.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE;bd.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;
        if(SUCCEEDED(hr))hr=d->CreateBlendState(&bd,&badgeBlend);
        D3D11_RASTERIZER_DESC rs{};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;
        if(SUCCEEDED(hr))hr=d->CreateRasterizerState(&rs,&fullscreenRaster);
        D3D11_BUFFER_DESC cb{};cb.ByteWidth=16;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        if(SUCCEEDED(hr))hr=d->CreateBuffer(&cb,nullptr,&constants);
        if(FAILED(hr)){Log("renderer init failed=%08X",hr);failed=true;return;}
        initialized=true;Log("renderer initialized native geometry capture; F4 DLSS, F5 motion, F6 reset");
    }
    void BeginOverlay(){++overlayDepth;}
    void EndOverlay(){if(overlayDepth)--overlayDepth;}
    bool InOverlay() const {return overlayDepth!=0;}
    // The game binds its final scene target inside pass_sprite. Resolve at the
    // first real UI draw, after those bindings are applied, before any glyphs.
    void ResolveBeforeUI(ID3D11DeviceContext* c,DrawCall call){
        if(!overlayDepth||resolvedBeforeUI||c!=context.Get()||!initialized||failed||captureOnly||traceOnly||(!enabled&&!debug))return;
        ComPtr<ID3D11RenderTargetView> target;c->OMGetRenderTargets(1,&target,nullptr);if(!target)return;
        ComPtr<ID3D11Resource> resource;target->GetResource(&resource);ComPtr<ID3D11Texture2D> color;resource.As(&color);if(!color)return;
        D3D11_TEXTURE2D_DESC desc{};color->GetDesc(&desc);
        if(desc.Width!=screenWidth||desc.Height!=screenHeight||desc.SampleDesc.Count!=1)return;
        ProcessOutput(color.Get(),call,framePublished,frameHeld);
        resolvedBeforeUI=framePublished||frameHeld;
        if(frame<12||frame%120==0)Log("pre-UI resolve frame=%llu target=%p format=%u published=%d held=%d",frame,color.Get(),desc.Format,framePublished,frameHeld);
    }
    void AfterDraw(ID3D11DeviceContext* c,bool indexed,UINT count,UINT start,INT base,DrawCall call) {
        if(c!=context.Get()||!initialized||failed||(!enabled&&!debug&&!traceOnly&&!captureOnly)||count<3||count>300000)return;
        ++totalDraws;
        if(overlayDepth){++uiDraws;return;}
        if(resolvedBeforeUI)return;
        D3D11_PRIMITIVE_TOPOLOGY topology;c->IAGetPrimitiveTopology(&topology);if(topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST&&topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)return;
        UINT vpCount=1;D3D11_VIEWPORT vp{};c->RSGetViewports(&vpCount,&vp);
        if(viewportDiagnostics++<16)Log("draw viewport=%gx%g origin=%g,%g screen=%ux%u topology=%u count=%u",vp.Width,vp.Height,vp.TopLeftX,vp.TopLeftY,screenWidth,screenHeight,topology,count);
        if(traceOnly)return;
        bool originNative=vp.TopLeftX==0&&vp.TopLeftY==0;
        bool originJittered=std::abs(vp.TopLeftX-jitterX)<.0001f&&std::abs(vp.TopLeftY-jitterY)<.0001f;
        if(vpCount!=1||vp.Width<640||vp.Height<360||(!originNative&&!originJittered)||UINT(vp.Width)!=screenWidth||UINT(vp.Height)!=screenHeight)return;
        ComPtr<ID3D11VertexShader> vs;c->VSGetShader(&vs,nullptr,nullptr);
        if(!vs||!captureStream)return;
        ID3D11RenderTargetView* rt[8]{};ComPtr<ID3D11DepthStencilView> depth;c->OMGetRenderTargets(8,rt,&depth);
        auto releaseRT=[&](){for(auto t:rt)if(t)t->Release();};
        if(!rt[0]){releaseRT();return;}
        ComPtr<ID3D11Resource> color;rt[0]->GetResource(&color);ComPtr<ID3D11Texture2D> colorTexture;color.As(&colorTexture);
        if(!colorTexture){releaseRT();return;}D3D11_TEXTURE2D_DESC colorDesc{};colorTexture->GetDesc(&colorDesc);
        if(colorDesc.Width!=UINT(vp.Width)||colorDesc.Height!=UINT(vp.Height)||colorDesc.SampleDesc.Count!=1){releaseRT();return;}
        if(!EnsureSurfaces(colorDesc.Width,colorDesc.Height)){releaseRT();failed=true;return;}
        if(clear){float zero[4]{},one[4]={1,1,1,1};c->ClearRenderTargetView(motion.rtv.Get(),zero);c->ClearRenderTargetView(mask.rtv.Get(),one);clear=false;}
        ComPtr<ID3D11GeometryShader> originalGS;c->GSGetShader(&originalGS,nullptr,nullptr);
        ComPtr<ID3D11HullShader> hs;c->HSGetShader(&hs,nullptr,nullptr);ComPtr<ID3D11DomainShader> ds;c->DSGetShader(&ds,nullptr,nullptr);
        ID3D11Buffer* so[4]{};c->SOGetTargets(4,so);bool hasSO=false;for(auto b:so){hasSO|=b!=nullptr;if(b)b->Release();}
        ComPtr<ID3D11PixelShader> originalPS;c->PSGetShader(&originalPS,nullptr,nullptr);
        ComPtr<ID3D11DepthStencilState> originalDepth;UINT stencil=0;c->OMGetDepthStencilState(&originalDepth,&stencil);
        ComPtr<ID3D11BlendState> originalBlend;float blend[4];UINT sampleMask;c->OMGetBlendState(&originalBlend,blend,&sampleMask);
        float zeros[4]{};
        if(!depth){releaseRT();return;}
        if(originalGS||hs||ds||hasSO){if(frame<3)Log("draw skipped stages gs=%p hs=%p ds=%p so=%d",originalGS.Get(),hs.Get(),ds.Get(),hasSO);releaseRT();return;}
        if(topology==D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST&&count%3){releaseRT();return;}
        UINT capturedVertices=topology==D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP?(count-2)*3:count;
        ComPtr<ID3D11Resource> dr;depth->GetResource(&dr);ComPtr<ID3D11Texture2D> dt;dr.As(&dt);
        D3D11_TEXTURE2D_DESC dd{};if(dt)dt->GetDesc(&dd);
        if(!dt||dd.Width!=width||dd.Height!=height||dd.SampleDesc.Count!=1){releaseRT();return;}
        D3D11_DEPTH_STENCIL_DESC depthState{};if(originalDepth)originalDepth->GetDesc(&depthState);else{depthState.DepthEnable=TRUE;depthState.DepthFunc=D3D11_COMPARISON_LESS;}
        if(!depthState.DepthEnable){releaseRT();return;}
        sceneDepth=dt;
        ComPtr<ID3D11Buffer> vb,ib;UINT stride=0,offset=0,indexOffset=0;DXGI_FORMAT indexFormat;
        c->IAGetVertexBuffers(0,1,&vb,&stride,&offset);c->IAGetIndexBuffer(&ib,&indexFormat,&indexOffset);
        ComPtr<ID3D11ShaderResourceView> texture;c->PSGetShaderResources(0,1,&texture);
        DrawKey key;key.vs=uintptr_t(vs.Get());key.vb=uintptr_t(vb.Get());key.ib=uintptr_t(ib.Get());key.texture=uintptr_t(texture.Get());key.count=count;key.start=start;key.stride=stride;key.offset=offset;key.base=base;key.indexOffset=indexOffset;key.indexFormat=indexFormat;key.topology=topology;
        key.occurrence=occurrences[key]++;
        auto found=history.find(key);
        if(found==history.end()){
            size_t bytes=size_t(capturedVertices)*16*2;
            if(historyBytes+bytes>historyLimit){releaseRT();return;}
            GeometryHistory record;record.bytes=bytes;
            D3D11_BUFFER_DESC bd{};bd.ByteWidth=capturedVertices*16;bd.BindFlags=D3D11_BIND_STREAM_OUTPUT|D3D11_BIND_SHADER_RESOURCE;
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;sv.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;sv.Buffer.NumElements=capturedVertices;
            for(int i=0;i<2;i++){HRESULT hr=device->CreateBuffer(&bd,nullptr,&record.buffers[i]);if(SUCCEEDED(hr))hr=device->CreateShaderResourceView(record.buffers[i].Get(),&sv,&record.srvs[i]);if(FAILED(hr)){Log("history buffer failed=%08X",hr);releaseRT();return;}}
            historyBytes+=bytes;found=history.emplace(key,std::move(record)).first;
        }
        auto& record=found->second;
        ComPtr<ID3D11ShaderResourceView> oldGSSRV;ComPtr<ID3D11Buffer> oldGSCB,oldPSCB;c->GSGetShaderResources(0,1,&oldGSSRV);c->GSGetConstantBuffers(0,1,&oldGSCB);c->PSGetConstantBuffers(0,1,&oldPSCB);
        UINT soOffset=0;auto target=record.buffers[record.write].Get();c->SOSetTargets(1,&target,&soOffset);
        c->OMSetRenderTargets(0,nullptr,nullptr);c->GSSetShader(captureStream.Get(),nullptr,0);c->PSSetShader(nullptr,nullptr,0);
        call(c,indexed,count,start,base);c->SOSetTargets(0,nullptr,nullptr);
        if(record.frame!=UINT64_MAX&&record.frame+1==frame){
            auto mvTarget=motion.rtv.Get();auto previous=record.srvs[1-record.write].Get();auto cb=constants.Get();
            c->OMSetRenderTargets(1,&mvTarget,depth.Get());c->OMSetDepthStencilState(equalDepth.Get(),0);c->OMSetBlendState(noBlend.Get(),zeros,0xffffffff);
            c->GSSetShader(motionGS.Get(),nullptr,0);c->GSSetShaderResources(0,1,&previous);c->GSSetConstantBuffers(0,1,&cb);c->PSSetShader(motionPS.Get(),nullptr,0);c->PSSetConstantBuffers(0,1,&cb);
            call(c,indexed,count,start,base);++historyDraws;
        }
        auto savedGS=oldGSSRV.Get();auto savedCB=oldGSCB.Get();c->GSSetShaderResources(0,1,&savedGS);c->GSSetConstantBuffers(0,1,&savedCB);
        auto savedPSCB=oldPSCB.Get();c->PSSetConstantBuffers(0,1,&savedPSCB);
        c->GSSetShader(originalGS.Get(),nullptr,0);c->PSSetShader(originalPS.Get(),nullptr,0);
        c->OMSetRenderTargets(8,rt,depth.Get());c->OMSetDepthStencilState(originalDepth.Get(),stencil);c->OMSetBlendState(originalBlend.Get(),blend,sampleMask);
        record.frame=frame;record.write=1-record.write;++nativeDraws;releaseRT();
    }
    void ProcessOutput(ID3D11Texture2D* back,DrawCall call,bool& published,bool& held){
        if(!back)return;D3D11_TEXTURE2D_DESC bd{};back->GetDesc(&bd);
        if(nativeDraws&&motion.texture&&(enabled||debug)&&!captureOnly){
            if(back&&bd.Width==width&&bd.Height==height&&bd.SampleDesc.Count==1){
                PipelineState original(context.Get());
                D3D11_TEXTURE2D_DESC previous{};if(backCopy.texture)backCopy.texture->GetDesc(&previous);
                if(!backCopy.texture||previous.Format!=bd.Format){backCopy={};backCopy.Create(device.Get(),width,height,bd.Format,D3D11_BIND_SHADER_RESOURCE);}
                if(backCopy.texture&&CopyDepth()){
                    context->CopyResource(backCopy.texture.Get(),back);
                    auto raw=backCopy.srv.Get();context->PSSetShaderResources(0,1,&raw);BindFullscreen(input.rtv.Get(),copyPS.Get(),width,height);call(context.Get(),false,3,0,0);
                    ID3D11ShaderResourceView* empty[4]{};context->PSSetShaderResources(0,4,empty);context->OMSetRenderTargets(0,nullptr,nullptr);
                    bool ok=debug||(enabled&&dlss.Ready(device.Get(),context.Get(),width,height)&&dlss.Run(context.Get(),input.texture.Get(),output.texture.Get(),depthCopy.Get(),motion.texture.Get(),reset,jitterX,jitterY));
                    if(!ok&&enabled){enabled=false;ShowToast();Log("DLSS unavailable; viewport jitter disabled for subsequent frames");}
                    if(ok){
                        ComPtr<ID3D11RenderTargetView> backRT;device->CreateRenderTargetView(back,nullptr,&backRT);
                        if(backRT){ID3D11ShaderResourceView* views[]={backCopy.srv.Get(),output.srv.Get(),mask.srv.Get(),motion.srv.Get()};context->PSSetShaderResources(0,4,views);BindFullscreen(backRT.Get(),debug?debugPS.Get():composePS.Get(),width,height);call(context.Get(),false,3,0,0);published=true;reset=false;if(!debug&&++dlssFrames==1)ShowToast();}
                    }
                    context->PSSetShaderResources(0,4,empty);context->OMSetRenderTargets(0,nullptr,nullptr);
                    if(published){
                        D3D11_TEXTURE2D_DESC cachedDesc{};if(cachedDisplay.texture)cachedDisplay.texture->GetDesc(&cachedDesc);
                        if(!cachedDisplay.texture||cachedDesc.Format!=bd.Format){cachedDisplay={};cachedDisplay.Create(device.Get(),width,height,bd.Format,D3D11_BIND_SHADER_RESOURCE);}
                        if(cachedDisplay.texture){context->CopyResource(cachedDisplay.texture.Get(),back);cachedDisplayValid=true;}
                    }
                }
            }
            if(!published)cachedDisplayValid=false;
        }else if(!nativeDraws&&cachedDisplayValid&&(enabled||debug)&&!captureOnly){
            if(back&&bd.Width==width&&bd.Height==height){
                PipelineState original(context.Get());
                D3D11_TEXTURE2D_DESC previous{};if(pausedRaw.texture)pausedRaw.texture->GetDesc(&previous);
                if(!pausedRaw.texture||previous.Format!=bd.Format){pausedRaw={};if(!pausedRaw.Create(device.Get(),width,height,bd.Format,D3D11_BIND_SHADER_RESOURCE))return;}
                context->CopyResource(pausedRaw.texture.Get(),back);
                auto raw=pausedRaw.srv.Get();context->PSSetShaderResources(0,1,&raw);BindFullscreen(input.rtv.Get(),copyPS.Get(),width,height);call(context.Get(),false,3,0,0);
                ID3D11ShaderResourceView* nullView=nullptr;context->PSSetShaderResources(0,1,&nullView);context->OMSetRenderTargets(0,nullptr,nullptr);
                ComPtr<ID3D11RenderTargetView> backRT;device->CreateRenderTargetView(back,nullptr,&backRT);
                if(backRT){
                    ID3D11ShaderResourceView* views[]={input.srv.Get(),cachedDisplay.srv.Get(),mask.srv.Get(),nullptr,backCopy.srv.Get()};
                    context->PSSetShaderResources(0,5,views);BindFullscreen(backRT.Get(),heldPS.Get(),width,height);call(context.Get(),false,3,0,0);
                    ID3D11ShaderResourceView* empty[5]{};context->PSSetShaderResources(0,5,empty);context->OMSetRenderTargets(0,nullptr,nullptr);held=true;
                }
            }
        }
    }
    void Present(IDXGISwapChain* swap,DrawCall call) {
        if(!initialized||failed)return;
        UINT keys=pendingKeys.exchange(0);
        if(!windowKeyEvents){
            DWORD foregroundPid=0;GetWindowThreadProcessId(GetForegroundWindow(),&foregroundPid);
            for(UINT i=0;i<3;++i){bool down=(GetAsyncKeyState(VK_F4+int(i))&0x8000)!=0;if(foregroundPid==GetCurrentProcessId()&&down&&!previousKeys[i])keys|=1u<<i;previousKeys[i]=down;}
        }
        if(keys&1){enabled=!enabled;reset=true;history.clear();historyBytes=0;PersistSetting(L"Enable",enabled);configuredFlags=(configuredFlags&~1)|(enabled?1:0);ShowToast();Log("DLSS toggle=%d",enabled);}
        if(keys&2){debug=!debug;PersistSetting(L"DebugMotion",debug);configuredFlags=(configuredFlags&~2)|(debug?2:0);ShowToast();Log("motion debug=%d",debug);}
        if(keys&4){history.clear();historyBytes=0;reset=true;Log("temporal history reset");}
        if(!resolvedBeforeUI&&!uiDraws){
            ComPtr<ID3D11Texture2D> back;swap->GetBuffer(0,IID_PPV_ARGS(&back));ProcessOutput(back.Get(),call,framePublished,frameHeld);
        }
        bool published=framePublished,held=frameHeld;
        RenderBadge(swap,call);
        if(frame<12||frame%120==0)Log("frame=%llu calls=%u geometry=%u with_history=%u ui=%u jittered=%u jitter=%g,%g cache_mb=%zu evaluate=%08X publish=%d held=%d extent=%ux%u",frame,totalDraws,nativeDraws,historyDraws,uiDraws,jitteredDraws,jitterX,jitterY,historyBytes/1024/1024,dlss.lastResult,published,held,width,height);
        for(auto i=history.begin();i!=history.end();){if(i->second.frame+120<frame){historyBytes-=i->second.bytes;i=history.erase(i);}else ++i;}
        if(!nativeDraws)reset=true;nativeDraws=historyDraws=uiDraws=totalDraws=jitteredDraws=0;resolvedBeforeUI=framePublished=frameHeld=false;jitterX=jitterY=0;occurrences.clear();clear=true;++frame;
        if(frame%120==0)ReadSettings();
    }
    void Resize(){dlss.Resize();history.clear();occurrences.clear();historyBytes=0;motion={};mask={};input={};output={};backCopy={};pausedRaw={};cachedDisplay={};cachedDisplayValid=false;depthCopy.Reset();depthSRV.Reset();sceneDepth.Reset();width=height=0;clear=true;reset=true;resolvedBeforeUI=framePublished=frameHeld=false;}
    void Shutdown(){enabled=debug=false;Resize();dlss.Shutdown(device.Get());}
    uint64_t SuccessfulDlssFrames() const {return dlssFrames;}
};
