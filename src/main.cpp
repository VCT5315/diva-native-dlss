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
INT STDMETHODCALLTYPE HookBeginEvent(ID3DUserDefinedAnnotation* annotation,LPCWSTR name){
    INT result=originalBeginEvent(annotation,name);
    if(stopped||replay||annotation!=gameAnnotation.Get())return result;
    bool foreground=name&&wcscmp(name,L"pass_sprite")==0;eventStack.push_back(foreground);
    if(foreground){renderer.BeginOverlay();static unsigned int hits=0;if(hits++<4)Log("GPU foreground event reached; pass_sprite executes before UI draw");}
    return result;
}
INT STDMETHODCALLTYPE HookEndEvent(ID3DUserDefinedAnnotation* annotation){
    if(!stopped&&!replay&&annotation==gameAnnotation.Get()&&!eventStack.empty()){
        if(eventStack.back())renderer.EndOverlay();eventStack.pop_back();
    }
    return originalEndEvent(annotation);
}
void* drawTargets[4]{};
std::vector<void*> hookTargets;
HWND gameWindow=nullptr;WNDPROC originalWindowProc=nullptr;
LRESULT CALLBACK GameWindowProc(HWND window,UINT message,WPARAM w,LPARAM l){
    // Bit 30 marks repeated key-down messages. The first press is queued even
    // if key-up happens before the next rendered frame. Alt+F4 passes through.
    if(!stopped&&message==WM_KEYDOWN&&!(l&(LPARAM(1)<<30))&&w>=VK_F4&&w<=VK_F6)renderer.QueueKey(UINT(w));
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
void UpdateExtent(IDXGISwapChain* s){
    ComPtr<ID3D11Texture2D> back;
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
extern "C" __declspec(dllexport) void Init(){if(started.exchange(true))return;Log("DivaNativeDLSS build 27 loaded; executed GPU foreground event; DLAA/NR before UI; no UI classification mask");}
// Optional integration boundary, also exercised by the GPU validation host.
extern "C" __declspec(dllexport) void DivaNativeDLSSBeginOverlay(){if(!stopped)renderer.BeginOverlay();}
extern "C" __declspec(dllexport) void DivaNativeDLSSEndOverlay(){if(!stopped)renderer.EndOverlay();}
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
