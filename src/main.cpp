#include "render.hpp"
#include "engine_ui.hpp"
#include <atomic>
#include "../third_party/minhook/include/MinHook.h"

namespace {
HMODULE module=nullptr;
std::mutex logMutex;
Renderer& renderer=*new Renderer();
std::atomic<bool> stopped{false},started{false};
ID3D11Device* gameDevice=nullptr;
ID3D11DeviceContext* gameContext=nullptr;
unsigned int gameContextDraws=0,otherContextDraws=0;
IDXGISwapChain* gameSwap=nullptr;
thread_local bool replay=false,replayInstanced=false;
thread_local UINT replayInstance=0;
using DrawIndexedFn=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,INT);
using DrawFn=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT);
using DrawIndexedInstancedFn=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,UINT,INT,UINT);
using DrawInstancedFn=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,UINT,UINT);
DrawIndexedFn originalIndexed=nullptr;
DrawFn originalDraw=nullptr;
DrawIndexedInstancedFn originalIndexedInstanced=nullptr;
DrawInstancedFn originalInstanced=nullptr;
using BeginEventFn=INT(STDMETHODCALLTYPE*)(ID3DUserDefinedAnnotation*,LPCWSTR);
using EndEventFn=INT(STDMETHODCALLTYPE*)(ID3DUserDefinedAnnotation*);
BeginEventFn originalBeginEvent=nullptr;EndEventFn originalEndEvent=nullptr;
ComPtr<ID3DUserDefinedAnnotation> gameAnnotation;
thread_local std::vector<bool> eventStack;
// Pairing counters. 'pass_sprite' is a per-frame annotation, so begins and ends
// must match; a divergence means the boundary the mod resolves at is wrong.
unsigned int annotationBegins=0,annotationEnds=0,foregroundBegins=0;
INT STDMETHODCALLTYPE HookBeginEvent(ID3DUserDefinedAnnotation* annotation,LPCWSTR name){
    INT result=originalBeginEvent(annotation,name);
    if(stopped||replay||annotation!=gameAnnotation.Get())return result;
    bool foreground=name&&wcscmp(name,L"pass_sprite")==0;eventStack.push_back(foreground);
    ++annotationBegins;if(foreground)++foregroundBegins;
    if(annotationBegins%600==0)Log("annotations begins=%u ends=%u foreground=%u stack=%zu",annotationBegins,annotationEnds,foregroundBegins,eventStack.size());
    if(foreground){renderer.BeginOverlay();static unsigned int hits=0;if(hits++<4)Log("GPU foreground event reached; pass_sprite executes before UI draw");}
    return result;
}
INT STDMETHODCALLTYPE HookEndEvent(ID3DUserDefinedAnnotation* annotation){
    if(!stopped&&!replay&&annotation==gameAnnotation.Get()&&!eventStack.empty()){
        if(eventStack.back())renderer.EndOverlay();eventStack.pop_back();++annotationEnds;
    }
    return originalEndEvent(annotation);
}
void* drawTargets[4]{};
std::vector<void*> hookTargets;
// ExecuteCommandList is vtable slot 58: 54 within ID3D11DeviceContext plus the
// 4 ID3D11DeviceChild methods and 3 IUnknown methods ahead of it. Counted from
// d3d11.h rather than remembered, because a wrong slot patches an unrelated
// function and kills the game.
using ExecuteCommandListFn=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11CommandList*,BOOL);
ExecuteCommandListFn originalExecuteCommandList=nullptr;
unsigned int executeCommandListCalls=0,executeCommandListOnGame=0;
void STDMETHODCALLTYPE HookExecuteCommandList(ID3D11DeviceContext* c,ID3D11CommandList* list,BOOL restore){
    ++executeCommandListCalls;if(c==gameContext)++executeCommandListOnGame;
    if(executeCommandListCalls%60==0)Log("ExecuteCommandList calls=%u onGame=%u ctx=%p game=%p",executeCommandListCalls,executeCommandListOnGame,c,gameContext);
    originalExecuteCommandList(c,list,restore);
}
HWND gameWindow=nullptr;WNDPROC originalWindowProc=nullptr;
// Mouse coordinates arrive in client pixels; the panel works in backbuffer
// pixels, so scale by the ratio between the two.
void QueueMouseEvent(HWND window,LPARAM l,bool down){
    RECT client{};if(!GetClientRect(window,&client))return;
    const int clientWidth=client.right-client.left,clientHeight=client.bottom-client.top;
    const UINT screenWidth=renderer.ScreenWidth(),screenHeight=renderer.ScreenHeight();
    if(clientWidth<=0||clientHeight<=0||!screenWidth||!screenHeight)return;
    const int x=int(short(LOWORD(l)))*int(screenWidth)/clientWidth;
    const int y=int(short(HIWORD(l)))*int(screenHeight)/clientHeight;
    if(down)renderer.QueueMouseDown(x,y);else renderer.QueueMouseMove(x,y);
}
LRESULT CALLBACK GameWindowProc(HWND window,UINT message,WPARAM w,LPARAM l){
    // Bit 30 marks repeated key-down messages. The first press is queued even
    // if key-up happens before the next rendered frame. Alt+F4 passes through.
    if(!stopped){
        if(message==WM_KEYDOWN&&!(l&(LPARAM(1)<<30))){
            renderer.QueueKey(UINT(w));
            // While the panel is open it owns the navigation keys, so the game
            // does not also scroll or confirm behind it. This only covers
            // window-message input; a game reading raw input directly may still
            // see those keys.
            if(renderer.OverlayVisible()){
                switch(w){
                case VK_UP:case VK_DOWN:case VK_LEFT:case VK_RIGHT:
                case VK_RETURN:case VK_ESCAPE:case VK_F7:
                    return 0;
                default:break;
                }
            }
        }
        else if(message==WM_MOUSEMOVE&&renderer.OverlayVisible())QueueMouseEvent(window,l,false);
        else if(message==WM_LBUTTONDOWN&&renderer.OverlayVisible())QueueMouseEvent(window,l,true);
    }
    return CallWindowProcW(originalWindowProc,window,message,w,l);
}

template<class F> bool Detour(void* target,void* replacement,F& original){
    void* trampoline=nullptr;
    auto status=MH_CreateHook(target,replacement,&trampoline);
    if(status!=MH_OK){Log("detour create failed=%d target=%p",status,target);return false;}
    original=reinterpret_cast<F>(trampoline);
    status=MH_EnableHook(target);
    Log("detour target=%p replacement=%p trampoline=%p enable=%d",target,replacement,trampoline,status);
    hookTargets.push_back(target);
    return status==MH_OK;
}
void ReplayDraw(ID3D11DeviceContext* c,bool indexed,UINT count,UINT start,INT base){
    const bool was=replay;replay=true;
    if(replayInstanced&&indexed)originalIndexedInstanced(c,count,1,start,base,replayInstance);
    else if(replayInstanced)originalInstanced(c,count,1,start,replayInstance);
    else if(indexed)originalIndexed(c,count,start,base);
    else originalDraw(c,count,start);
    replay=was;
}
void PrepareDraw(ID3D11DeviceContext* c,Renderer::ViewportRestore& viewport){
    if(replay)return;
    // Every draw reaches here, but everything downstream is gated on
    // `c == gameContext`. If the game renders on a context other than the one DML
    // handed us, the mod then looks perfectly healthy while processing almost
    // nothing - which is exactly the 1080p symptom. Count both to tell them apart.
    if(c==gameContext){if(++gameContextDraws%600==0)Log("draw contexts game=%u other=%u",gameContextDraws,otherContextDraws);}
    else if(++otherContextDraws%600==0)Log("draw contexts game=%u other=%u this=%p game=%p",gameContextDraws,otherContextDraws,c,gameContext);
    if(renderer.InOverlay()){replay=true;renderer.ResolveBeforeUI(c,&ReplayDraw);replay=false;}
    renderer.BeforeDraw(c,viewport);
}
void DrawDiagnostic(ID3D11DeviceContext* c,const char* kind,UINT count){
    static std::atomic<unsigned int> n{0};
    if(n.fetch_add(1)<24)Log("native %s context=%p game=%p replay=%d count=%u",kind,c,gameContext,replay,count);
}
void STDMETHODCALLTYPE HookDrawIndexed(ID3D11DeviceContext* c,UINT count,UINT start,INT base){
    Renderer::ViewportRestore viewport;PrepareDraw(c,viewport);
    originalIndexed(c,count,start,base);
    if(!replay){DrawDiagnostic(c,"indexed",count);replay=true;renderer.AfterDraw(c,true,count,start,base,&ReplayDraw);replay=false;}
}
void STDMETHODCALLTYPE HookDraw(ID3D11DeviceContext* c,UINT count,UINT start){
    Renderer::ViewportRestore viewport;PrepareDraw(c,viewport);
    originalDraw(c,count,start);
    if(!replay){DrawDiagnostic(c,"draw",count);replay=true;renderer.AfterDraw(c,false,count,start,0,&ReplayDraw);replay=false;}
}
void STDMETHODCALLTYPE HookDrawIndexedInstanced(ID3D11DeviceContext* c,UINT count,UINT instances,UINT start,INT base,UINT first){
    Renderer::ViewportRestore viewport;PrepareDraw(c,viewport);
    originalIndexedInstanced(c,count,instances,start,base,first);
    if(!replay&&c==gameContext){DrawDiagnostic(c,"indexed instanced",count);replay=true;replayInstanced=true;
        for(UINT i=0;i<std::min(instances,64u);++i){replayInstance=first+i;renderer.AfterDraw(c,true,count,start,base,&ReplayDraw);}
        replayInstanced=false;replay=false;
    }
}
void STDMETHODCALLTYPE HookDrawInstanced(ID3D11DeviceContext* c,UINT count,UINT instances,UINT start,UINT first){
    Renderer::ViewportRestore viewport;PrepareDraw(c,viewport);
    originalInstanced(c,count,instances,start,first);
    if(!replay&&c==gameContext){DrawDiagnostic(c,"instanced",count);replay=true;replayInstanced=true;
        for(UINT i=0;i<std::min(instances,64u);++i){replayInstance=first+i;renderer.AfterDraw(c,false,count,start,0,&ReplayDraw);}
        replayInstanced=false;replay=false;
    }
}
using OMSetRenderTargetsFn=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,ID3D11RenderTargetView* const*,ID3D11DepthStencilView*);
OMSetRenderTargetsFn originalOMSetRenderTargets=nullptr;
// Record only, never substitute - yet. Option C (render the 3D at low resolution,
// keep the UI at 4K) means swapping the game's scene render target for a smaller
// one, and the upscale has to happen exactly when the game stops drawing into it
// and starts reading it. Getting that boundary wrong makes post-processing read a
// stale surface, which looks like corruption rather than a crash. So first: the
// real binding sequence.
void STDMETHODCALLTYPE HookOMSetRenderTargets(ID3D11DeviceContext* c,UINT numViews,ID3D11RenderTargetView* const* ppRTVs,ID3D11DepthStencilView* pDSV){
    static void* lastRT=nullptr;static unsigned int binds=0;static unsigned int logged=0;
    void* current=(numViews&&ppRTVs)?static_cast<void*>(ppRTVs[0]):nullptr;
    if(c==gameContext&&!replay&&current!=lastRT&&logged<120){
        lastRT=current;++logged;++binds;
        D3D11_TEXTURE2D_DESC rt{};bool haveRT=false;
        if(current){ComPtr<ID3D11Resource> r;ppRTVs[0]->GetResource(&r);ComPtr<ID3D11Texture2D> t;r.As(&t);if(t){t->GetDesc(&rt);haveRT=true;}}
        D3D11_TEXTURE2D_DESC dp{};bool haveD=false;
        if(pDSV){ComPtr<ID3D11Resource> r;pDSV->GetResource(&r);ComPtr<ID3D11Texture2D> t;r.As(&t);if(t){t->GetDesc(&dp);haveD=true;}}
        Log("OMSetRT #%u n=%u rt=%p %ux%u fmt=%u dsv=%d %ux%u fmt=%u inUI=%d",
            binds,numViews,current,haveRT?rt.Width:0,haveRT?rt.Height:0,haveRT?UINT(rt.Format):0,
            pDSV?1:0,haveD?dp.Width:0,haveD?dp.Height:0,haveD?UINT(dp.Format):0,renderer.InOverlay()?1:0);
    }
    originalOMSetRenderTargets(c,numViews,ppRTVs,pDSV);
}
void UpdateExtent(IDXGISwapChain* s){    ComPtr<ID3D11Texture2D> back;
    if(SUCCEEDED(s->GetBuffer(0,IID_PPV_ARGS(&back)))){
        D3D11_TEXTURE2D_DESC bd{};back->GetDesc(&bd);renderer.SetScreenExtent(bd.Width,bd.Height);
        Log("game backbuffer extent=%ux%u format=%u",bd.Width,bd.Height,bd.Format);
    }
}
void Install(ID3D11Device* d,ID3D11DeviceContext* c,IDXGISwapChain* s){
    if(!d||!c||!s||gameDevice||stopped)return;
    gameDevice=d;gameContext=c;gameSwap=s;
    auto status=MH_Initialize();
    if(status!=MH_OK&&status!=MH_ERROR_ALREADY_INITIALIZED){Log("MinHook initialize failed=%d",status);return;}
    auto cv=*reinterpret_cast<void***>(c);
    drawTargets[0]=cv[12];drawTargets[1]=cv[13];drawTargets[2]=cv[20];drawTargets[3]=cv[21];
    bool ok=true;
    ok=Detour(cv[12],reinterpret_cast<void*>(&HookDrawIndexed),originalIndexed)&&ok;
    ok=Detour(cv[13],reinterpret_cast<void*>(&HookDraw),originalDraw)&&ok;
    ok=Detour(cv[20],reinterpret_cast<void*>(&HookDrawIndexedInstanced),originalIndexedInstanced)&&ok;
    ok=Detour(cv[21],reinterpret_cast<void*>(&HookDrawInstanced),originalInstanced)&&ok;
    if(!ok){for(auto t:hookTargets)MH_DisableHook(t);Log("required draw hooks failed; renderer bypassed");return;}
    auto foreground=FindForegroundPass(GetModuleHandleW(nullptr));
    if(foreground)Log("verified engine pass_sprite=%p; commands deferred to annotation execution",foreground);
    if(SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&gameAnnotation)))&&gameAnnotation){
        auto av=*reinterpret_cast<void***>(gameAnnotation.Get());
        bool beginOK=Detour(av[3],reinterpret_cast<void*>(&HookBeginEvent),originalBeginEvent);
        bool endOK=Detour(av[4],reinterpret_cast<void*>(&HookEndEvent),originalEndEvent);
        if(beginOK&&endOK)Log("GPU annotation boundaries hooked; annotation=%p; DLAA/NR before executed pass_sprite",gameAnnotation.Get());
        else {MH_DisableHook(av[3]);MH_DisableHook(av[4]);Log("GPU annotation hook unavailable; no UI classification mask");}
    }else Log("GPU annotation interface unavailable; no UI classification mask");
    replay=true;renderer.Initialize(d,c);replay=false;UpdateExtent(s);
    DXGI_SWAP_CHAIN_DESC desc{};
    if(SUCCEEDED(s->GetDesc(&desc))&&desc.OutputWindow){
        gameWindow=desc.OutputWindow;originalWindowProc=reinterpret_cast<WNDPROC>(GetWindowLongPtrW(gameWindow,GWLP_WNDPROC));SetLastError(0);auto previous=SetWindowLongPtrW(gameWindow,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(&GameWindowProc));
        if(previous){originalWindowProc=reinterpret_cast<WNDPROC>(previous);renderer.UseWindowKeyEvents(true);Log("game window key events installed; first press only");}
        else {gameWindow=nullptr;Log("window key events unavailable=%lu; using key-down edges",GetLastError());}
    }
    Log("DML D3DInit attached device=%p context=%p swapchain=%p feature=%X",d,c,s,d->GetFeatureLevel());
}
}
std::wstring ModDirectory(){wchar_t p[MAX_PATH]{};GetModuleFileNameW(module,p,MAX_PATH);std::wstring s(p);return s.substr(0,s.find_last_of(L"\\/"));}
std::wstring GameDirectory(){wchar_t p[MAX_PATH]{};GetModuleFileNameW(nullptr,p,MAX_PATH);std::wstring s(p);return s.substr(0,s.find_last_of(L"\\/"));}
void Log(const char* format,...){std::lock_guard<std::mutex> lock(logMutex);char b[2048]{};va_list a;va_start(a,format);vsnprintf_s(b,sizeof(b),_TRUNCATE,format,a);va_end(a);FILE* f=nullptr;_wfopen_s(&f,(ModDirectory()+L"\\DivaNativeDLSS.log").c_str(),L"a");if(f){fprintf(f,"%s\n",b);fclose(f);}}
extern "C" __declspec(dllexport) void Init(){if(started.exchange(true))return;Log("DivaNativeDLSS build 28 loaded; switchable DLSS/FSR4 backends; F7 configuration panel");}
// Optional integration boundary, also exercised by the GPU validation host.
extern "C" __declspec(dllexport) void DivaNativeDLSSBeginOverlay(){if(!stopped)renderer.BeginOverlay();}
extern "C" __declspec(dllexport) void DivaNativeDLSSEndOverlay(){if(!stopped)renderer.EndOverlay();}
// Small surface used by the validation host to drive the configuration panel.
extern "C" __declspec(dllexport) int DivaNativeDLSSOverlayVisible(){return renderer.OverlayVisible()?1:0;}
extern "C" __declspec(dllexport) void DivaNativeDLSSQueueKey(unsigned int key){if(!stopped)renderer.QueueKey(key);}
extern "C" __declspec(dllexport) int DivaNativeDLSSSelectedBackend(){return renderer.Config().backend;}
extern "C" __declspec(dllexport) int DivaNativeDLSSBackendReady(){return renderer.BackendReady()?1:0;}
extern "C" __declspec(dllexport) void D3DInit(IDXGISwapChain* s,ID3D11Device* d,ID3D11DeviceContext* c){Install(d,c,s);}
extern "C" __declspec(dllexport) void OnFrame(IDXGISwapChain* s){
    if(stopped||replay||s!=gameSwap)return;
    static unsigned int frames=0;
    if(frames++<3||frames%120==0){auto cv=*reinterpret_cast<void***>(gameContext);Log("DML OnFrame=%u context=%p targets=%p,%p,%p,%p changed=%d",frames,gameContext,cv[12],cv[13],cv[20],cv[21],cv[12]!=drawTargets[0]||cv[13]!=drawTargets[1]||cv[20]!=drawTargets[2]||cv[21]!=drawTargets[3]);}
    replay=true;renderer.Present(s,&ReplayDraw);replay=false;
}
extern "C" __declspec(dllexport) void OnResize(IDXGISwapChain* s){if(!stopped&&s==gameSwap){renderer.Resize();UpdateExtent(s);}}
extern "C" __declspec(dllexport) unsigned long long DivaNativeDLSSSuccessfulFrames(){return stopped?0:renderer.SuccessfulDlssFrames();}
extern "C" __declspec(dllexport) void DivaNativeDLSSShutdown(){
    if(stopped.exchange(true))return;
    if(gameWindow&&reinterpret_cast<WNDPROC>(GetWindowLongPtrW(gameWindow,GWLP_WNDPROC))==&GameWindowProc)SetWindowLongPtrW(gameWindow,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(originalWindowProc));
    // Only our hooks are disabled; other mods may also use MinHook.
    for(auto t:hookTargets)MH_DisableHook(t);
    renderer.Shutdown();delete &renderer;
    for(auto t:hookTargets)MH_RemoveHook(t);
    gameAnnotation.Reset();
    Log("own hooks and NGX released");
}
BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH){module=h;DisableThreadLibraryCalls(h);}return TRUE;}
