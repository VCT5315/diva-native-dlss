#pragma once
#include "common.hpp"
#include "state.hpp"
#include "upscaler_ngx.hpp"
#include "upscaler_fsr.hpp"
#include "overlay.hpp"
#include <cstdint>
#include <cstring>
#include <cmath>
#include <atomic>
#include <memory>

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
    DXGI_FORMAT depthCopyFormat=DXGI_FORMAT_UNKNOWN;
    std::unordered_map<DrawKey,GeometryHistory,KeyHash> history;
    std::unordered_map<DrawKey,UINT,KeyHash> occurrences;
    size_t historyBytes=0;size_t historyLimit=128*1024*1024;
    UINT width=0,height=0,screenWidth=0,screenHeight=0;bool initialized=false,failed=false,clear=true,reset=true;
    bool hdrActive=false;DXGI_FORMAT workingFormat=DXGI_FORMAT_R8G8B8A8_UNORM;int autoColorSpace=1;
    uint64_t frame=0;
    UINT nativeDraws=0,historyDraws=0,uiDraws=0,overlayDepth=0;
    bool resolvedBeforeUI=false,framePublished=false,frameHeld=false;
    uint64_t publishedFrames=0;
    UINT totalDraws=0,jitteredDraws=0,hookDraws=0,postPasses=0;unsigned int viewportDiagnostics=0;
    float jitterX=0,jitterY=0;bool cachedDisplayValid=false;
    Surface cachedDisplay;
    Surface badge;ComPtr<ID3D11BlendState> badgeBlend;
    uint64_t toastUntil=0;bool badgeDirty=true;
    std::atomic<UINT> pendingKeys{0};bool windowKeyEvents=false;bool previousKeys[3]{};
    // Keys and mouse events are queued from the window procedure and consumed
    // on the render thread, so the overlay is only ever touched from one thread.
    std::mutex inputMutex;std::vector<UINT> pendingRawKeys;
    std::atomic<bool> overlayVisible{false};
    std::atomic<int> pendingMouseX{0},pendingMouseY{0};
    std::atomic<bool> pendingMouseMove{false},pendingMouseDown{false};

    int appliedRevision=-1;
    int backendBeforeOff=int(Backend::Auto);
    std::wstring overlayBackendName,overlayStatus,overlayDetail;

    Settings settings;
    ConfigOverlay overlay;
    std::unique_ptr<IUpscaler> upscaler;
    Backend activeBackend=Backend::Off;
    int backendFailures=0;bool backendGaveUp=false;
    UINT64 lastFrameTick=0;double smoothedFrameMs=16.6667;
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
cbuffer Blit:register(b0){float2 SourceScale;float2 BlitPad;};
// CopyPS is a 1:1 load at the target's pixel coordinates, so it cannot be used to
// hand a smaller surface back to a larger one - the coordinates run past the
// source and the result is black. ScalePS maps target pixels into source pixels.
float4 ScalePS(V v):SV_Target{int2 src=int2(v.p.xy*SourceScale);return Source.Load(int3(src,0));}
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
    // True when the game presents high dynamic range.
    static bool IsHdrFormat(DXGI_FORMAT format) {
        return format==DXGI_FORMAT_R10G10B10A2_UNORM||format==DXGI_FORMAT_R10G10B10A2_TYPELESS||
               format==DXGI_FORMAT_R16G16B16A16_FLOAT||format==DXGI_FORMAT_R16G16B16A16_TYPELESS||
               format==DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
    }
    // The container does not imply the transfer function, and getting this
    // wrong is very visible: feeding linear scRGB to the upscaler while
    // declaring PQ crushes every highlight.
    //   1 sRGB (SDR, gamma encoded)
    //   2 PQ / HDR10 (R10G10B10A2 containers are PQ encoded)
    //   3 linear scRGB (float containers hold linear light)
    static int AutoColorSpace(DXGI_FORMAT format) {
        switch (format)
        {
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return 2;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:    return 3;
        default:                                return 1;
        }
    }
    // `sourceFormat` is the format the game renders into, which is what tells
    // HDR apart from SDR. The working chain is 16-bit float for HDR so the
    // upscaler never sees a clamped 8-bit image.
    bool EnsureSurfaces(UINT w,UINT h,DXGI_FORMAT sourceFormat) {
        const bool hdr=IsHdrFormat(sourceFormat);
        const DXGI_FORMAT working=hdr?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM;
        if(width==w&&height==h&&motion.texture&&workingFormat==working)return true;
        Resize();width=w;height=h;hdrActive=hdr;workingFormat=working;
        autoColorSpace=AutoColorSpace(sourceFormat);
        UINT rt=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        if(!motion.Create(device.Get(),w,h,DXGI_FORMAT_R16G16_FLOAT,rt)||!mask.Create(device.Get(),w,h,DXGI_FORMAT_R8_UNORM,rt)||
           !input.Create(device.Get(),w,h,working,rt)||
           !output.Create(device.Get(),w,h,working,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS))return false;
        UpdateFrameConstants();
        Log("render surfaces extent=%ux%u source_format=%u working_format=%u hdr=%d colour_space=%d history_limit_mb=%zu",w,h,unsigned(sourceFormat),unsigned(working),hdr?1:0,autoColorSpace,historyLimit/1024/1024);return true;
    }
    bool CopyDepth() {
        if(!sceneDepth)return false;
        D3D11_TEXTURE2D_DESC desc{};sceneDepth->GetDesc(&desc);
        if(!CoversScreen(desc.Width,desc.Height)||desc.SampleDesc.Count!=1)return false;
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
            depthCopyFormat=desc.Format;
        }
        context->CopyResource(depthCopy.Get(),sceneDepth.Get());return true;
    }
    static float Halton(UINT index,UINT base){float value=0,fraction=1;while(index){fraction/=float(base);value+=fraction*float(index%base);index/=base;}return value;}
    void UpdateFrameConstants(){float values[4]={float(width),float(height),jitterX,jitterY};context->UpdateSubresource(constants.Get(),0,nullptr,values,0,0);}

    // [FSR] Quality -> the ratio between the upscaler's input and its output.
    // 1.0 is Native AA, where input and output are the same surface size.
    //
    // DISABLED. The ratio modes are implemented end to end (the bridge creates
    // smaller inputs, the backend declares renderSize != upscaleSize, and the frame
    // is downsampled through ScalePS), but enabling them produced three problems
    // that are not fixed: the picture was clipped into a corner, the tier made no
    // visible difference, and changing it at runtime could reset the display
    // driver - the last one points at the shared-surface rebuild on a size change,
    // where D3D12 may still reference surfaces the D3D11 side just released.
    // Until that is redesigned, always report a 1.0 ratio so the whole path stays
    // dormant and behaviour is exactly the known-good Native AA.
    float UpScaleRatio() const { return 1.0f; }
    UINT RenderWidth()  const {const float r=UpScaleRatio();return r<=1.0f?width :((UINT(float(width )/r)+1u)&~1u);}
    UINT RenderHeight() const {const float r=UpScaleRatio();return r<=1.0f?height:((UINT(float(height)/r)+1u)&~1u);}

    // Ratio-mode inputs. The game always renders at full resolution, so the frame
    // and the motion vectors are downsampled here on the way into the upscaler;
    // that is the only cost the quality tiers remove.
    Surface scaleColor,scaleMotion;
    UINT scaleColorW=0,scaleColorH=0;
    bool PrepareDownscaled(ID3D11DeviceContext* c,DrawCall call,UINT rw,UINT rh){
        static unsigned int attemptLog=0;
        if(attemptLog++<6)Log("internal ratio: attempt %ux%u call=%d",rw,rh,call?1:0);
        if(!call||rw<64||rh<64){if(attemptLog<8)Log("internal ratio: rejected call=%d size",call?1:0);return false;}
        if(!scaleColor.texture||scaleColorW!=rw||scaleColorH!=rh){
            scaleColor={};scaleMotion={};scaleColorW=scaleColorH=0;
            if(!scaleColor.Create(device.Get(),rw,rh,workingFormat,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET)){Log("internal ratio: colour target create failed %ux%u fmt=%u",rw,rh,unsigned(workingFormat));return false;}
            if(!scaleMotion.Create(device.Get(),rw,rh,DXGI_FORMAT_R16G16_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET)){Log("internal ratio: motion target create failed");scaleColor={};return false;}
            scaleColorW=rw;scaleColorH=rh;
            Log("internal ratio: downscale targets %ux%u from %ux%u",rw,rh,width,height);
        }
        // ScalePS maps target pixels into source pixels; CopyPS cannot, since it
        // loads by target coordinate and would read past the smaller source.
        UINT savedVpCount=1;D3D11_VIEWPORT savedVp{};c->RSGetViewports(&savedVpCount,&savedVp);
        // BindFullscreen rebinds the render target, shaders, blend/depth/raster state
        // and the viewport. This runs in the middle of the game's frame, so all of it
        // has to be handed back - restoring only the viewport left the game drawing
        // its UI into the small colour target, which showed up as the picture being
        // clipped into the top-left corner.
        PipelineState savedPipeline(c);
        float blit[4]={float(rw)/float(width),float(rh)/float(height),0,0};
        c->UpdateSubresource(constants.Get(),0,nullptr,blit,0,0);
        ID3D11Buffer* cb=constants.Get();c->PSSetConstantBuffers(0,1,&cb);
        ID3D11ShaderResourceView* src=backCopy.srv.Get();
        if(!src){Log("internal ratio: no game frame SRV");return false;}
        c->PSSetShaderResources(0,1,&src);
        BindFullscreen(scaleColor.rtv.Get(),scalePS.Get(),rw,rh);call(c,false,3,0,0);
        ID3D11ShaderResourceView* none[1]{};c->PSSetShaderResources(0,1,none);
        src=motion.srv.Get();
        if(src){
            c->PSSetShaderResources(0,1,&src);
            BindFullscreen(scaleMotion.rtv.Get(),scalePS.Get(),rw,rh);call(c,false,3,0,0);
            c->PSSetShaderResources(0,1,none);
        }else Log("internal ratio: no motion SRV");
        // BindFullscreen leaves a render-resolution viewport behind. This runs in the
        // middle of the frame, so without restoring it every later draw - the game's
        // UI in particular, which comes after the pass_sprite resolve - is clipped
        // into the top-left corner of the full-size target.
        if(savedVpCount)c->RSSetViewports(1,&savedVp);
        ID3D11Buffer* nocb[1]{};c->PSSetConstantBuffers(0,1,nocb);
        return true;
    }

    // ---- Option C: run the game's 3D at a lower internal resolution ----------
    // The binding trace shows the 3D pass binding a full-screen MRT (n=2 at
    // 3840x2160, depth R32_TYPELESS) and only afterwards running bloom over a mip
    // chain that reads it back. Redirecting those bindings to smaller surfaces is
    // what actually cuts the game's own rasterisation cost - downscaling a finished
    // 4K frame saves only the upscaler's share and leaves all the 4K raster work in
    // place. The small surfaces are copied back into the game's originals the moment
    // it binds anything else, which is the observed hand-back point.
    //
    // Off by default. This rewrites binding state the game reasons about, and
    // handing back at the wrong moment corrupts the image instead of crashing, so
    // every transition is logged and gated.
    bool redirectActive=false;
    UINT redirectCount=0,smallW=0,smallH=0;
    DrawCall redirectCall=nullptr;
    ComPtr<ID3D11RenderTargetView> gameRT[4];
    ComPtr<ID3D11DepthStencilView> gameDS;
    ComPtr<ID3D11Texture2D> smallColor[4];
    ComPtr<ID3D11RenderTargetView> smallRTV[4];
    ComPtr<ID3D11ShaderResourceView> smallSRV[4];
    ComPtr<ID3D11Texture2D> smallDepth;
    ComPtr<ID3D11DepthStencilView> smallDSV;
    ComPtr<ID3D11PixelShader> scalePS;
    unsigned int redirectBegins=0,redirectEnds=0,redirectSkips=0;

    static bool ViewSize(ID3D11View* view,UINT& w,UINT& h){
        if(!view)return false;
        ComPtr<ID3D11Resource> r;view->GetResource(&r);ComPtr<ID3D11Texture2D> t;r.As(&t);
        if(!t)return false;
        D3D11_TEXTURE2D_DESC d{};t->GetDesc(&d);w=d.Width;h=d.Height;return true;
    }
    bool EnsureSmallTargets(UINT w,UINT h,UINT n,ID3D11RenderTargetView* const* rtvs,ID3D11DepthStencilView* dsv){
        if(smallRTV[0]&&smallDSV&&smallW==w&&smallH==h)return true;
        for(UINT i=0;i<4;i++){smallColor[i].Reset();smallRTV[i].Reset();smallSRV[i].Reset();}
        smallDepth.Reset();smallDSV.Reset();smallW=smallH=0;
        for(UINT i=0;i<n;i++){
            ComPtr<ID3D11Resource> r;rtvs[i]->GetResource(&r);ComPtr<ID3D11Texture2D> t;r.As(&t);
            if(!t)return false;
            D3D11_TEXTURE2D_DESC d{};t->GetDesc(&d);
            d.Width=w;d.Height=h;d.MipLevels=1;d.ArraySize=1;d.SampleDesc.Count=1;d.SampleDesc.Quality=0;
            d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=0;d.MiscFlags=0;
            d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
            if(FAILED(device->CreateTexture2D(&d,nullptr,&smallColor[i])))return false;
            if(FAILED(device->CreateRenderTargetView(smallColor[i].Get(),nullptr,&smallRTV[i])))return false;
            if(FAILED(device->CreateShaderResourceView(smallColor[i].Get(),nullptr,&smallSRV[i])))return false;
        }
        ComPtr<ID3D11Resource> dr;dsv->GetResource(&dr);ComPtr<ID3D11Texture2D> dt;dr.As(&dt);
        if(!dt)return false;
        D3D11_TEXTURE2D_DESC dd{};dt->GetDesc(&dd);
        // Keep the typeless format: a typed depth format here would give the game a
        // different resource than the one it created and bound.
        dd.Width=w;dd.Height=h;dd.MipLevels=1;dd.ArraySize=1;dd.SampleDesc.Count=1;dd.SampleDesc.Quality=0;
        dd.Usage=D3D11_USAGE_DEFAULT;dd.CPUAccessFlags=0;dd.MiscFlags=0;dd.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        if(FAILED(device->CreateTexture2D(&dd,nullptr,&smallDepth)))return false;
        if(FAILED(device->CreateDepthStencilView(smallDepth.Get(),nullptr,&smallDSV)))return false;
        smallW=w;smallH=h;
        return true;
    }
    // Returns true when the smaller targets were bound instead of the game's, so the
    // caller must not perform its own bind.
public:
    bool RedirectBind(ID3D11DeviceContext* c,UINT n,ID3D11RenderTargetView* const* rtvs,ID3D11DepthStencilView* dsv,DrawCall call){
        if(redirectActive)RedirectEnd(c);
        return false;   // option C was abandoned; kept only so the removal stays reviewable
        if(overlayDepth||n==0||n>4||!rtvs||!rtvs[0]||!dsv){
            if(redirectSkips++<8)Log("internal scale skip n=%u rt=%d dsv=%d inUI=%d",n,rtvs&&rtvs[0]?1:0,dsv?1:0,overlayDepth?1:0);
            return false;
        }
        UINT w=0,h=0;
        if(!ViewSize(rtvs[0],w,h)||w!=screenWidth||h!=screenHeight){
            if(redirectSkips++<8)Log("internal scale skip size=%ux%u screen=%ux%u",w,h,screenWidth,screenHeight);
            return false;
        }
        const UINT sw=screenWidth,sh=screenHeight;   // unreachable: kept for review
        if(!EnsureSmallTargets(sw,sh,n,rtvs,dsv)){
            if(redirectSkips++<8)Log("internal scale skip: small targets failed");
            return false;
        }
        ID3D11RenderTargetView* bind[4]{};
        for(UINT i=0;i<n;i++)bind[i]=smallRTV[i].Get();
        c->OMSetRenderTargets(n,bind,smallDSV.Get());
        for(UINT i=0;i<n;i++)gameRT[i]=rtvs[i];
        gameDS=dsv;redirectCall=call;redirectActive=true;redirectCount=n;
        if(redirectBegins++<8)Log("internal scale begin %ux%u -> %ux%u n=%u",screenWidth,screenHeight,sw,sh,n);
        return true;
    }
    void RedirectEnd(ID3D11DeviceContext* c){
        if(!redirectActive)return;
        const UINT n=redirectCount;redirectActive=false;
        if(!smallSRV[0]||!copyPS||!redirectCall)return;
        PipelineState saved(c);
        // Map target pixels into source pixels. CopyPS loads by target coordinate,
        // so feeding it a smaller source reads past the end and renders black.
        float blit[4]={float(smallW)/float(screenWidth),float(smallH)/float(screenHeight),0,0};
        c->UpdateSubresource(constants.Get(),0,nullptr,blit,0,0);
        ID3D11Buffer* cb=constants.Get();c->PSSetConstantBuffers(0,1,&cb);
        for(UINT i=0;i<n;i++){
            if(!gameRT[i]||!smallSRV[i])continue;
            ID3D11ShaderResourceView* v=smallSRV[i].Get();
            c->PSSetShaderResources(0,1,&v);
            BindFullscreen(gameRT[i].Get(),scalePS.Get(),screenWidth,screenHeight);
            redirectCall(c,false,3,0,0);
            ID3D11ShaderResourceView* none[1]{};
            c->PSSetShaderResources(0,1,none);
        }
        ID3D11Buffer* nocb[1]{};c->PSSetConstantBuffers(0,1,nocb);
        c->OMSetRenderTargets(0,nullptr,nullptr);
        if(redirectEnds++<8)Log("internal scale end (%u targets)",n);
    }
private:

    void ReadSettings(){
        // Load() only bumps the revision when a value really changed.
        settings.Load();
        ApplySettingsIfChanged();
    }

    void ApplySettingsIfChanged(){
        if(settings.revision==appliedRevision)return;
        appliedRevision=settings.revision;
        const size_t limit=size_t(std::clamp(settings.historyMB,32,512))*1024*1024;
        if(limit!=historyLimit){historyLimit=limit;Resize();}
        SelectBackend();
        ShowToast();
        Log("settings applied rev=%d backend=%d debug=%d trace=%d capture=%d history=%zuMB sharpness=%d%% nonlinear=%d mvScale=%d%% mvSignY=%d",
            appliedRevision,settings.backend,settings.debugMotion,settings.traceOnly,settings.captureOnly,
            historyLimit/1024/1024,settings.fsrSharpnessPercent,settings.fsrNonLinear,settings.fsrMvScalePercent,settings.fsrMvSignY);
    }
    void ShowToast(){toastUntil=GetTickCount64()+3500;badgeDirty=true;}

    // Resolves Backend::Auto and (re)creates the backend object when the
    // selection changed. Cheap to call: it returns immediately when nothing moved.
    void SelectBackend(){
        Backend want=Backend(settings.backend);
        if(want==Backend::Auto){
            if(!FsrUpscaler::FindUpscalerDll(settings).empty())want=Backend::Fsr4;
#if DIVA_ENABLE_NGX
            else want=Backend::Dlss;
#else
            else want=Backend::Off;
#endif
        }
        if(upscaler&&want==activeBackend)return;
        if(upscaler){upscaler->Shutdown();upscaler.reset();}
        activeBackend=want;
        backendFailures=0;backendGaveUp=false;
        if(!device)return;
        if(want==Backend::Fsr4){
            auto* fsr=new FsrUpscaler();
            if(!fsr->Initialize(device.Get(),settings))Log("FSR backend unavailable: %ls",fsr->StatusText());
            upscaler.reset(fsr);
        }
#if DIVA_ENABLE_NGX
        else if(want==Backend::Dlss)upscaler.reset(CreateNgxUpscaler());
#endif
        Log("backend selected=%d (auto=%d)",int(activeBackend),settings.backend);
    }
    void RenderBadge(IDXGISwapChain* swap,DrawCall call){
        // Deliberately NOT suppressed by an open panel: the toast is the only
        // sign of life the user has if the panel itself fails to appear.
        if(!settings.showStatus||GetTickCount64()>toastUntil)return;
        PipelineState original(context.Get());
        if(badgeDirty){
            static wchar_t message[160]{};
            if(settings.debugMotion)wcscpy_s(message,L"运动矢量预览   F5 返回");
            else{
                const bool on=activeBackend!=Backend::Off&&!backendGaveUp;
                swprintf_s(message,L"%ls %ls   F4 切换   F7 设置",BackendName(activeBackend),on?L"已开启":L"已关闭");
            }
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
        D3D11_TEXTURE2D_DESC td{};badge.texture->GetDesc(&td);
        UINT badgeVpCount=1;D3D11_VIEWPORT badgeVpSaved{};context->RSGetViewports(&badgeVpCount,&badgeVpSaved);
        BindFullscreen(rt.Get(),badgePS.Get(),td.Width,td.Height);
        D3D11_VIEWPORT vp{20,20,float(td.Width),float(td.Height),0,1};context->RSSetViewports(1,&vp);float factor[4]{};context->OMSetBlendState(badgeBlend.Get(),factor,0xffffffff);
        auto text=badge.srv.Get();context->PSSetShaderResources(0,1,&text);call(context.Get(),false,3,0,0);
        // The toast is drawn at (20,20) with its own small viewport. This runs in the
        // middle of the game's frame, so without restoring it the game's remaining UI
        // is clipped into that corner - which is exactly what a cropped picture with
        // only the top-left visible looks like.
        if(badgeVpCount)context->RSSetViewports(1,&badgeVpSaved);

        // Control experiment: the toast is confirmed visible in-game, so probing
        // it with the same technique tells us whether a readback on this path is
        // trustworthy at all.
        static unsigned int badgeProbes=0;
        if(badgeProbes<3&&back)
        {
            ++badgeProbes;
            D3D11_TEXTURE2D_DESC probeDesc{};back->GetDesc(&probeDesc);
            D3D11_TEXTURE2D_DESC sd=probeDesc;
            sd.Width=4;sd.Height=4;sd.MipLevels=1;sd.ArraySize=1;
            sd.Usage=D3D11_USAGE_STAGING;sd.BindFlags=0;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;sd.MiscFlags=0;
            ComPtr<ID3D11Texture2D> staging;
            if(SUCCEEDED(device->CreateTexture2D(&sd,nullptr,&staging)))
            {
                const D3D11_BOX box{30,30,0,34,34,1};
                context->CopySubresourceRegion(staging.Get(),0,0,0,0,back.Get(),0,&box);
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)))
                {
                    const unsigned char* p=static_cast<const unsigned char*>(mapped.pData);
                    Log("badge probe @30,30 = %u %u %u %u (toast is dark; scene at 30,30 would be clear colour)",
                        p[0],p[1],p[2],p[3]);
                    context->Unmap(staging.Get(),0);
                }
            }
        }
    }
public:
    void UseWindowKeyEvents(bool value){windowKeyEvents=value;}
    // Called from the game's window procedure. Everything is queued and applied
    // on the render thread instead of touching the overlay from here.
    void QueueKey(UINT key){
        static std::atomic<unsigned int> seen{0};
        if(seen.fetch_add(1)<40)Log("key down vk=%u (overlayKey=%d overlayVisible=%d)",key,settings.overlayKey,overlay.Visible()?1:0);
        if(key>=VK_F4&&key<=VK_F6){pendingKeys.fetch_or(1u<<(key-VK_F4));return;}
        if(key==UINT(settings.overlayKey)||overlayVisible.load()||overlay.Visible()){
            std::lock_guard<std::mutex> lock(inputMutex);
            if(pendingRawKeys.size()<64)pendingRawKeys.push_back(key);
        }
    }
    void QueueMouseMove(int x,int y){pendingMouseX.store(x);pendingMouseY.store(y);pendingMouseMove.store(true);}
    void QueueMouseDown(int x,int y){pendingMouseX.store(x);pendingMouseY.store(y);pendingMouseDown.store(true);}
    bool OverlayVisible() const {return overlayVisible.load();}
    Settings& Config(){return settings;}
    struct ViewportRestore {
        ID3D11DeviceContext* c=nullptr;D3D11_VIEWPORT viewport{};
        ~ViewportRestore(){if(c)c->RSSetViewports(1,&viewport);}
    };
    // The game oversizes some viewports and render targets: at 1080p it renders
    // 1920x1088 while the swap chain is 1920x1080 (the extra rows clip). Requiring
    // exact equality against the screen size rejected every draw there, which is
    // why the mod silently did nothing at 1080p while working at 4K.
    bool CoversScreen(UINT w,UINT h) const {
        // While the 3D is redirected the effective target is the smaller surface,
        // so every size test downstream has to compare against that instead.
        const UINT sw=redirectActive?smallW:screenWidth,sh=redirectActive?smallH:screenHeight;
        return w>=sw&&h>=sh&&w<=sw+16&&h<=sh+16;
    }
    bool CoversScreen(const D3D11_VIEWPORT& vp) const {return CoversScreen(UINT(vp.Width),UINT(vp.Height));}
    void BeforeDraw(ID3D11DeviceContext* c,ViewportRestore& restore){
        if(c!=context.Get()||!initialized||failed||activeBackend==Backend::Off||settings.traceOnly||settings.captureOnly||overlayDepth||resolvedBeforeUI)return;
        UINT n=1;D3D11_VIEWPORT vp{};c->RSGetViewports(&n,&vp);
        if(n!=1||vp.TopLeftX!=0||vp.TopLeftY!=0||!CoversScreen(vp)||vp.Width<640||vp.Height<360)return;
        ComPtr<ID3D11DepthStencilView> depth;c->OMGetRenderTargets(0,nullptr,&depth);if(!depth)return;
        ComPtr<ID3D11DepthStencilState> state;c->OMGetDepthStencilState(&state,nullptr);D3D11_DEPTH_STENCIL_DESC sd{};if(state){state->GetDesc(&sd);if(!sd.DepthEnable)return;}
        ComPtr<ID3D11Resource> resource;depth->GetResource(&resource);ComPtr<ID3D11Texture2D> texture;resource.As(&texture);if(!texture)return;
        D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);if(!CoversScreen(td.Width,td.Height)||td.SampleDesc.Count!=1)return;
        // D3D11 accepts fractional viewport origins, so this adds real subpixel
        // samples without requiring a TAA implementation or editing game shaders.
        UINT phase=UINT(frame%8)+1;jitterX=Halton(phase,2)-.5f;jitterY=Halton(phase,3)-.5f;
        // The game's viewport is expressed in screen pixels; the redirected target is
        // smaller, so the viewport and the subpixel jitter both scale with it.
        float sx=1.0f,sy=1.0f;
        if(redirectActive&&screenWidth&&screenHeight){sx=float(smallW)/float(screenWidth);sy=float(smallH)/float(screenHeight);}
        restore.c=c;restore.viewport=vp;
        vp.TopLeftX=vp.TopLeftX*sx+jitterX*sx;vp.TopLeftY=vp.TopLeftY*sy+jitterY*sy;
        vp.Width*=sx;vp.Height*=sy;
        c->RSSetViewports(1,&vp);++jitteredDraws;
        if(width&&height)UpdateFrameConstants();
    }
    void SetScreenExtent(UINT w,UINT h){screenWidth=w;screenHeight=h;overlay.SetScreenSize(w,h);}
    UINT ScreenWidth() const {return screenWidth;}
    UINT ScreenHeight() const {return screenHeight;}
    bool BackendReady() const {return upscaler&&activeBackend!=Backend::Off&&!backendGaveUp;}
    void Initialize(ID3D11Device* d,ID3D11DeviceContext* c) {
        if(initialized)return;device=d;context=c;
        settings.Load();
        appliedRevision=settings.revision;
        historyLimit=size_t(std::clamp(settings.historyMB,32,512))*1024*1024;
        auto gs=Compile(motionCode,"MotionGS","gs_5_0"),ps=Compile(motionCode,"MotionPS","ps_5_0"),capture=Compile(motionCode,"CaptureGS","gs_5_0");
        auto vs=Compile(fullscreenCode,"FullVS","vs_5_0"),cp=Compile(fullscreenCode,"CopyPS","ps_5_0"),sp=Compile(fullscreenCode,"ScalePS","ps_5_0"),np=Compile(fullscreenCode,"ComposePS","ps_5_0"),dp=Compile(fullscreenCode,"DebugPS","ps_5_0"),hp=Compile(fullscreenCode,"HeldPS","ps_5_0"),bp=Compile(fullscreenCode,"BadgePS","ps_5_0");
        if(!gs||!ps||!vs||!cp||!sp||!np||!dp||!capture||!hp||!bp){failed=true;return;}
        HRESULT hr=d->CreateGeometryShader(gs->GetBufferPointer(),gs->GetBufferSize(),nullptr,&motionGS);
        D3D11_SO_DECLARATION_ENTRY streamEntry{0,"SV_Position",0,0,4,0};UINT streamStride=16;
        if(SUCCEEDED(hr))hr=d->CreateGeometryShaderWithStreamOutput(capture->GetBufferPointer(),capture->GetBufferSize(),&streamEntry,1,&streamStride,1,D3D11_SO_NO_RASTERIZED_STREAM,nullptr,&captureStream);
        if(SUCCEEDED(hr))hr=d->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&motionPS);
        if(SUCCEEDED(hr))hr=d->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&fullscreenVS);
        if(SUCCEEDED(hr))hr=d->CreatePixelShader(cp->GetBufferPointer(),cp->GetBufferSize(),nullptr,&copyPS);
        if(SUCCEEDED(hr))hr=d->CreatePixelShader(sp->GetBufferPointer(),sp->GetBufferSize(),nullptr,&scalePS);
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
        initialized=true;
        SelectBackend();
        overlay.Initialize(device.Get());
        Log("renderer initialized native geometry capture; F4 toggle, F5 motion, F6 reset, F7 settings");
    }
    void BeginOverlay(){++overlayDepth;}
    void EndOverlay(){if(overlayDepth)--overlayDepth;}
    bool InOverlay() const {return overlayDepth!=0;}
    // The game binds its final scene target inside pass_sprite. Resolve at the
    // first real UI draw, after those bindings are applied, before any glyphs.
    void ResolveBeforeUI(ID3D11DeviceContext* c,DrawCall call){
        if(!overlayDepth||resolvedBeforeUI||c!=context.Get()||!initialized||failed||settings.captureOnly||settings.traceOnly||(activeBackend==Backend::Off&&!settings.debugMotion))return;
        ComPtr<ID3D11RenderTargetView> target;c->OMGetRenderTargets(1,&target,nullptr);if(!target)return;
        ComPtr<ID3D11Resource> resource;target->GetResource(&resource);ComPtr<ID3D11Texture2D> color;resource.As(&color);if(!color)return;
        D3D11_TEXTURE2D_DESC desc{};color->GetDesc(&desc);
        if(desc.Width!=screenWidth||desc.Height!=screenHeight||desc.SampleDesc.Count!=1)return;
        ProcessOutput(color.Get(),call,framePublished,frameHeld);
        resolvedBeforeUI=framePublished||frameHeld;
        if(frame<12||frame%12==0)Log("pre-UI resolve frame=%llu target=%p format=%u published=%d held=%d",frame,color.Get(),desc.Format,framePublished,frameHeld);
    }
    void AfterDraw(ID3D11DeviceContext* c,bool indexed,UINT count,UINT start,INT base,DrawCall call) {
        // Counted before every filter, so the log can distinguish "the draw hook
        // never fired" from "the draw was rejected below". Without it a PV that
        // reports calls=4 is indistinguishable from a dead hook.
        ++hookDraws;
        // Sample what the hook actually sees, unfiltered: viewport size and vertex
        // count. That separates "the game draws elsewhere" from "the filters here
        // reject every draw".
        if(frame>20&&hookDraws<=3&&frame%600==0){
            UINT sv=1;D3D11_VIEWPORT sbvp{};c->RSGetViewports(&sv,&sbvp);
            Log("hook sample frame=%llu n=%u vp=%.0fx%.0f@%.0f,%.0f count=%u indexed=%d",frame,hookDraws,sbvp.Width,sbvp.Height,sbvp.TopLeftX,sbvp.TopLeftY,count,indexed?1:0);
        }
        if(c!=context.Get()||!initialized||failed||(activeBackend==Backend::Off&&!settings.debugMotion&&!settings.traceOnly&&!settings.captureOnly)||count<3||count>300000)return;
        ++totalDraws;
        if(overlayDepth){++uiDraws;return;}
        if(resolvedBeforeUI)return;
        // Post-processing passes: full-screen draws with no depth bound, running
        // between the 3D pass and pass_sprite. The game always applies either MLAA or
        // FXAA here and offers no way to disable it. Logging them identifies which one
        // does the AA - toggling MLAA against FXAA in the game's options changes that
        // pass's pixel shader pointer, which is what pins it down. Removing the AA
        // then means replacing that pass with a pass-through copy rather than skipping
        // it, because skipping leaves the next pass reading a stale buffer.
        if(!overlayDepth){
            UINT pvCount=1;D3D11_VIEWPORT pv{};c->RSGetViewports(&pvCount,&pv);
            ComPtr<ID3D11DepthStencilView> postDepth;c->OMGetRenderTargets(0,nullptr,&postDepth);
            if(!postDepth&&pvCount==1&&pv.Width>=float(screenWidth)&&pv.Height>=float(screenHeight)){
                ++postPasses;
                if(postPasses<=10||frame%600==0){
                    ComPtr<ID3D11PixelShader> postPS;c->PSGetShader(&postPS,nullptr,nullptr);
                    ComPtr<ID3D11VertexShader> postVS;c->VSGetShader(&postVS,nullptr,nullptr);
                    ComPtr<ID3D11ShaderResourceView> postSrc;c->PSGetShaderResources(0,1,&postSrc);
                    ComPtr<ID3D11Resource> postRes;if(postSrc)postSrc->GetResource(&postRes);
                    ComPtr<ID3D11Texture2D> postTex;postRes.As(&postTex);
                    D3D11_TEXTURE2D_DESC postDesc{};if(postTex)postTex->GetDesc(&postDesc);
                    Log("post pass frame=%llu #%u ps=%p vs=%p count=%u vp=%.0fx%.0f src=%ux%u fmt=%u",
                        frame,postPasses,postPS.Get(),postVS.Get(),count,pv.Width,pv.Height,
                        postDesc.Width,postDesc.Height,unsigned(postDesc.Format));
                }
            }
        }
        D3D11_PRIMITIVE_TOPOLOGY topology;c->IAGetPrimitiveTopology(&topology);
        UINT vpCount=1;D3D11_VIEWPORT vp{};c->RSGetViewports(&vpCount,&vp);
        if(viewportDiagnostics++<16)Log("draw viewport=%gx%g origin=%g,%g screen=%ux%u topology=%u count=%u",vp.Width,vp.Height,vp.TopLeftX,vp.TopLeftY,screenWidth,screenHeight,topology,count);
        bool originNative=vp.TopLeftX==0&&vp.TopLeftY==0;
        bool originJittered=std::abs(vp.TopLeftX-jitterX)<.0001f&&std::abs(vp.TopLeftY-jitterY)<.0001f;
        // Report which gate rejects a draw. BeforeDraw accepts the 3D pass
        // (jittered>0) while this capture path rejects it (geometry==0), so the two
        // gates disagree and only an explicit reason identifies which check is
        // responsible.
        static unsigned int rejectLog=0;
        auto reject=[&](const char* why){
            if(rejectLog++<24)Log("capture reject '%s' vp=%gx%g origin=%g,%g count=%u topo=%u jit=%g,%g screen=%ux%u native=%d jittered=%d",why,vp.Width,vp.Height,vp.TopLeftX,vp.TopLeftY,count,UINT(topology),jitterX,jitterY,screenWidth,screenHeight,originNative?1:0,originJittered?1:0);
        };
        if(topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST&&topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP){reject("topology");return;}
        if(settings.traceOnly){reject("traceOnly");return;}
        if(vpCount!=1||vp.Width<640||vp.Height<360||(!originNative&&!originJittered)||!CoversScreen(vp)){reject("viewport");return;}
        ComPtr<ID3D11VertexShader> vs;c->VSGetShader(&vs,nullptr,nullptr);
        if(!vs||!captureStream){reject(vs?"captureStream missing":"no vertex shader");return;}
        ID3D11RenderTargetView* rt[8]{};ComPtr<ID3D11DepthStencilView> depth;c->OMGetRenderTargets(8,rt,&depth);
        auto releaseRT=[&](){for(auto t:rt)if(t)t->Release();};
        if(!rt[0]){reject("no render target");releaseRT();return;}
        ComPtr<ID3D11Resource> color;rt[0]->GetResource(&color);ComPtr<ID3D11Texture2D> colorTexture;color.As(&colorTexture);
        if(!colorTexture){reject("rt not texture2d");releaseRT();return;}D3D11_TEXTURE2D_DESC colorDesc{};colorTexture->GetDesc(&colorDesc);
        // Pin the working chain to the SCREEN size, never to whatever render target
        // this particular pass happens to bind. At 1080p the 3D pass is 1920x1088
        // while the scene target is 1920x1080; sizing the surfaces from the pass
        // made them rebuild on alternate frames, which collapsed the frame rate.
        // The screen size is constant for a resolution, so no pass can move it.
        if(!CoversScreen(colorDesc.Width,colorDesc.Height)||colorDesc.SampleDesc.Count!=1){
            if(rejectLog++<24)Log("capture reject 'rt size' rt=%ux%u samples=%u screen=%ux%u",colorDesc.Width,colorDesc.Height,colorDesc.SampleDesc.Count,screenWidth,screenHeight);
            releaseRT();return;}
        if(!EnsureSurfaces(screenWidth,screenHeight,colorDesc.Format)){releaseRT();failed=true;return;}
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
        // The game pads 1080 lines up to 1088 (1080 is not a multiple of 16), so the
        // depth buffer is 1920x1088 while the screen is 1920x1080. Requiring the
        // depth to equal the working size rejected every capture at 1080p only, and
        // silently - there was no reject log on this path.
        if(!dt||!CoversScreen(dd.Width,dd.Height)||dd.SampleDesc.Count!=1){releaseRT();return;}
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
        const bool preview=settings.debugMotion!=0;
        const bool wantsUpscale=activeBackend!=Backend::Off&&!backendGaveUp&&!preview;
        if(nativeDraws&&motion.texture&&(wantsUpscale||preview)&&!settings.captureOnly){
            if(back&&bd.Width==width&&bd.Height==height&&bd.SampleDesc.Count==1){
                PipelineState original(context.Get());
                D3D11_TEXTURE2D_DESC previous{};if(backCopy.texture)backCopy.texture->GetDesc(&previous);
                if(!backCopy.texture||previous.Format!=bd.Format){backCopy={};backCopy.Create(device.Get(),width,height,bd.Format,D3D11_BIND_SHADER_RESOURCE);}
                if(backCopy.texture&&CopyDepth()){
                    context->CopyResource(backCopy.texture.Get(),back);
                    // The frame copy is already in the working format whenever the
                    // back buffer matches it (both RGBA16F in HDR, both RGBA8 in
                    // SDR), so routing it through `input` costs a whole redundant
                    // 4K fullscreen pass. At 3840x2160 HDR that is 132 MB of
                    // read+write traffic per frame; hand the copy straight over.
                    // Reverted to the pre-optimisation path: always route the frame
                    // copy through `input`. The shortcut that skipped this pass saved
                    // 132 MB/frame but changed the render path, and the configuration
                    // that was verified stable had it. Stability over bandwidth.
                    const bool directColor=false;
                    if(!directColor){auto raw=backCopy.srv.Get();context->PSSetShaderResources(0,1,&raw);BindFullscreen(input.rtv.Get(),copyPS.Get(),width,height);call(context.Get(),false,3,0,0);}
                    ID3D11ShaderResourceView* empty[4]{};context->PSSetShaderResources(0,4,empty);context->OMSetRenderTargets(0,nullptr,nullptr);

                    bool ok=false;
                    if(preview)ok=true;   // motion-vector visualisation only
                    else if(upscaler)
                    {
                        // Ratio modes need the upscaler's inputs at the smaller render
                        // size, so build them here. If that fails we fall back to the
                        // full-size path, which keeps Native AA behaviour intact.
                        const UINT rw=RenderWidth(),rh=RenderHeight();
                        const bool downscaled=(rw<width&&rh<height)&&PrepareDownscaled(context.Get(),call,rw,rh);
                        ID3D11Texture2D* colorTex=downscaled?scaleColor.texture.Get():(directColor?backCopy.texture.Get():input.texture.Get());
                        ID3D11Texture2D* motionTex=downscaled?scaleMotion.texture.Get():motion.texture.Get();
                        UpscaleFrame request;
                        request.device=device.Get();request.context=context.Get();
                        request.color=colorTex;
                        request.output=output.texture.Get();
                        request.depth=depthCopy.Get();request.depthSrv=depthSRV.Get();request.depthFormat=depthCopyFormat;
                        request.motion=motionTex;
                        request.width=width;request.height=height;request.reset=reset;
                        request.renderWidth=downscaled?rw:width;request.renderHeight=downscaled?rh:height;
                        request.jitterX=jitterX;request.jitterY=jitterY;
                        request.frameTimeMs=float(smoothedFrameMs);
                        request.hdr=hdrActive;
                        // 0 means auto: resolve it here so the backend never has
                        // to guess a transfer function from a container format.
                        request.colorSpace=settings.fsrColorSpace?settings.fsrColorSpace:autoColorSpace;
                        if(upscaler->Ready(request))ok=upscaler->Run(request);
                        if(ok)backendFailures=0;
                        else if(++backendFailures>=5){
                            backendGaveUp=true;ShowToast();
                            Log("backend %d failed %d times; frames left untouched until the setting changes",int(activeBackend),backendFailures);
                        }
                    }
                    if(ok){
                        // `mask` is only ever cleared to 1 (line 409) and nothing
                        // renders into it, so ComposePS reduces to
                        // lerp(source, output, 1) == output. When the working
                        // format matches the back buffer that makes the whole
                        // fullscreen compose pass redundant - a straight copy
                        // saves 240 MB of read+write traffic per frame at
                        // 3840x2160 HDR. The draw is kept for the motion-vector
                        // preview and for any format mismatch.
                        // Reverted: always compose through the shader rather than
                        // CopyResource, matching the verified-stable configuration.
                        if(true){
                            ComPtr<ID3D11RenderTargetView> backRT;device->CreateRenderTargetView(back,nullptr,&backRT);
                            if(backRT){ID3D11ShaderResourceView* views[]={backCopy.srv.Get(),output.srv.Get(),mask.srv.Get(),motion.srv.Get()};context->PSSetShaderResources(0,4,views);BindFullscreen(backRT.Get(),preview?debugPS.Get():composePS.Get(),width,height);call(context.Get(),false,3,0,0);published=true;reset=false;if(!preview&&++publishedFrames==1)ShowToast();}
                        }else{
                            context->CopyResource(back,output.texture.Get());
                            published=true;reset=false;if(++publishedFrames==1)ShowToast();
                        }
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
        }else if(!nativeDraws&&cachedDisplayValid&&(wantsUpscale||preview)&&!settings.captureOnly){
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
    void FillOverlayStats(ConfigOverlay::Stats& stats) {
        overlayBackendName=BackendName(activeBackend);
        overlayStatus=upscaler?upscaler->StatusText():(activeBackend==Backend::Off?L"超分已关闭":L"未初始化");
        if(auto* fsr=dynamic_cast<FsrUpscaler*>(upscaler.get())){
            const unsigned long long bytes=fsr->GpuMemoryUsageBytes();
            wchar_t buffer[128]{};
            if(bytes)swprintf_s(buffer,L"FSR 显存 %llu MB",bytes/1024/1024);
            else wcscpy_s(buffer,L"FSR 显存 -");
            overlayDetail=buffer;
        }else if(backendGaveUp)overlayDetail=L"后端连续失败，已暂停处理";
        else overlayDetail.clear();

        stats.backendName=overlayBackendName.c_str();
        stats.backendStatus=overlayStatus.c_str();
        stats.detail=overlayDetail.c_str();
        stats.frameMs=smoothedFrameMs;
        stats.fps=smoothedFrameMs>0.0?1000.0/smoothedFrameMs:0.0;
        stats.upscaledFrames=publishedFrames;
        stats.nativeDraws=nativeDraws;
        stats.motionDraws=historyDraws;
        stats.uiDraws=uiDraws;
        stats.historyBytes=historyBytes;
        stats.width=width;stats.height=height;stats.hdr=hdrActive;
        stats.lastResult=upscaler?upscaler->lastResult:0;
    }

    void Present(IDXGISwapChain* swap,DrawCall call) {
        if(!initialized||failed)return;

        const UINT64 now=GetTickCount64();
        if(lastFrameTick){const double delta=double(now-lastFrameTick);smoothedFrameMs=smoothedFrameMs*0.9+delta*0.1;}
        lastFrameTick=now;

        UINT keys=pendingKeys.exchange(0);
        std::vector<UINT> rawKeys;
        {std::lock_guard<std::mutex> lock(inputMutex);rawKeys.swap(pendingRawKeys);}

        for(UINT key:rawKeys){
            // F4/F5/F6 stay global even while the panel is open. Otherwise an
            // open panel silently swallows them, and if the panel is not being
            // drawn the whole mod looks dead.
            if(key>=VK_F4&&key<=VK_F6){keys|=1u<<(key-VK_F4);continue;}
            if(overlay.Visible()){overlay.OnKeyDown(key,settings);continue;}
            if(key==UINT(settings.overlayKey)){overlay.SetVisible(true);Log("overlay opened by vk=%u",key);continue;}
        }
        overlayVisible.store(overlay.Visible());

        if(!windowKeyEvents){
            DWORD foregroundPid=0;GetWindowThreadProcessId(GetForegroundWindow(),&foregroundPid);
            for(UINT i=0;i<3;++i){bool down=(GetAsyncKeyState(VK_F4+int(i))&0x8000)!=0;if(foregroundPid==GetCurrentProcessId()&&down&&!previousKeys[i])keys|=1u<<i;previousKeys[i]=down;}
        }
        // F4 keeps its original job: turn super resolution on and off.
        if(keys&1){
            if(settings.backend==int(Backend::Off))settings.backend=backendBeforeOff;
            else{backendBeforeOff=settings.backend;settings.backend=int(Backend::Off);}
            settings.Save();settings.revision++;Log("backend toggle=%d",settings.backend);
        }
        if(keys&2){settings.debugMotion=!settings.debugMotion;settings.Save();settings.revision++;Log("motion debug=%d",settings.debugMotion);}
        if(keys&4){history.clear();historyBytes=0;reset=true;Log("temporal history reset");}

        if(overlay.Visible()){
            if(pendingMouseMove.exchange(false))overlay.OnMouseMove(pendingMouseX.load(),pendingMouseY.load());
            if(pendingMouseDown.exchange(false))overlay.OnMouseButton(pendingMouseX.load(),pendingMouseY.load());
        }
        if(overlay.request.resetHistory){overlay.request.resetHistory=false;history.clear();historyBytes=0;reset=true;Log("history reset from panel");}
        if(overlay.request.reloadSettings){overlay.request.reloadSettings=false;settings.Load();}
        ApplySettingsIfChanged();

        // Announce the backend once, on the very first frame, whatever the state.
        // A config left at "Off" is otherwise indistinguishable from a broken mod:
        // nothing on screen, no error, and the log only reveals it if you know
        // which counter to look at. That ambiguity cost several test rounds.
        static bool backendAnnounced=false;
        if(!backendAnnounced){backendAnnounced=true;ShowToast();
            Log("startup backend=%d (0=auto 1=fsr4 2=dlss 3=off) upscaler=%s",
                int(activeBackend),upscaler?"present":"missing");}
        if(!resolvedBeforeUI&&!uiDraws){
            ComPtr<ID3D11Texture2D> back;swap->GetBuffer(0,IID_PPV_ARGS(&back));ProcessOutput(back.Get(),call,framePublished,frameHeld);
        }
        bool published=framePublished,held=frameHeld;
        RenderBadge(swap,call);
        // Draw the panel LAST, after the toast. Earlier builds drew it before
        // the toast in the same Present and the pixels never survived, while the
        // toast's identical write did; making the panel the final write before
        // present removes whatever was clobbering it.
        if(overlay.Visible()){
            ConfigOverlay::Stats stats;FillOverlayStats(stats);
            PipelineState saved(context.Get());
            overlay.Render(context.Get(),swap,settings,stats,call);
        }
        if(frame<12||frame%12==0)Log("frame=%llu hooks=%u calls=%u geometry=%u with_history=%u ui=%u jittered=%u jitter=%g,%g cache_mb=%zu backend=%d publish=%d held=%d extent=%ux%u",frame,hookDraws,totalDraws,nativeDraws,historyDraws,uiDraws,jitteredDraws,jitterX,jitterY,historyBytes/1024/1024,int(activeBackend),published,held,width,height);
        for(auto i=history.begin();i!=history.end();){if(i->second.frame+120<frame){historyBytes-=i->second.bytes;i=history.erase(i);}else ++i;}
        if(!nativeDraws)reset=true;nativeDraws=historyDraws=uiDraws=totalDraws=jitteredDraws=hookDraws=postPasses=0;overlayDepth=0;resolvedBeforeUI=framePublished=frameHeld=false;jitterX=jitterY=0;occurrences.clear();clear=true;++frame;
        if(frame%120==0)ReadSettings();
    }
    void Resize(){if(upscaler)upscaler->Invalidate();history.clear();occurrences.clear();historyBytes=0;motion={};mask={};input={};output={};backCopy={};pausedRaw={};cachedDisplay={};cachedDisplayValid=false;depthCopy.Reset();depthSRV.Reset();sceneDepth.Reset();depthCopyFormat=DXGI_FORMAT_UNKNOWN;width=height=0;hdrActive=false;workingFormat=DXGI_FORMAT_R8G8B8A8_UNORM;autoColorSpace=1;clear=true;reset=true;resolvedBeforeUI=framePublished=frameHeld=false;}
    void Shutdown(){settings.debugMotion=0;Resize();if(upscaler){upscaler->Shutdown();upscaler.reset();}activeBackend=Backend::Off;}
    uint64_t SuccessfulDlssFrames() const {return publishedFrames;}
};
