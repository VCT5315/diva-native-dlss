#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <algorithm>
#include <cstdio>
#include <cstdarg>
#include "../third_party/ngx/include/nvsdk_ngx.h"
using Microsoft::WRL::ComPtr;
void Log(const char* format,...);
std::wstring ModDirectory();
std::wstring GameDirectory();
inline unsigned int SafeCreate(ID3D11DeviceContext* c,NVSDK_NGX_Parameter* p,NVSDK_NGX_Handle** h) {
    __try { return NVSDK_NGX_D3D11_CreateFeature(c,NVSDK_NGX_Feature_SuperSampling,p,h); }
    __except(EXCEPTION_EXECUTE_HANDLER){Log("NGX create exception=%08X",GetExceptionCode());return 0xBAD00002;}
}
inline unsigned int SafeEvaluate(ID3D11DeviceContext* c,NVSDK_NGX_Handle* h,NVSDK_NGX_Parameter* p) {
    __try { return NVSDK_NGX_D3D11_EvaluateFeature(c,h,p,nullptr); }
    __except(EXCEPTION_EXECUTE_HANDLER){Log("NGX evaluate exception=%08X",GetExceptionCode());return 0xBAD00002;}
}
class Dlss {
    NVSDK_NGX_Parameter* params=nullptr;NVSDK_NGX_Handle* handle=nullptr;
    bool initialized=false,attempted=false;UINT width=0,height=0;
public:
    unsigned int lastResult=0;
    bool Ready(ID3D11Device* device,ID3D11DeviceContext* context,UINT w,UINT h) {
        if(handle&&width==w&&height==h)return true;
        if(attempted&&!handle)return false;
        ReleaseFeature();attempted=true;
        if(!initialized){
            auto game=GameDirectory(),cache=ModDirectory()+L"\\ngx-cache";CreateDirectoryW(cache.c_str(),nullptr);
            const wchar_t* paths[]={game.c_str()};NVSDK_NGX_FeatureCommonInfo info{};info.PathListInfo={paths,1};
            lastResult=NVSDK_NGX_D3D11_Init_with_ProjectID("607d2856-6385-4bc7-9b22-1cd2e4555356",NVSDK_NGX_ENGINE_TYPE_CUSTOM,"DivaNativeDLSS-0.1",cache.c_str(),device,&info,NVSDK_NGX_Version_API);
            Log("NGX init=%08X project=DivaNativeDLSS runtime_path=%ls",lastResult,game.c_str());
            if(lastResult!=1)return false;initialized=true;
        }
        if(!params){lastResult=NVSDK_NGX_D3D11_AllocateParameters(&params);Log("NGX parameters=%08X",lastResult);if(lastResult!=1||!params)return false;}
        params->Set("Width",w);params->Set("Height",h);params->Set("OutWidth",w);params->Set("OutHeight",h);
        params->Set("PerfQualityValue",5); // Full-resolution DLAA, no internal-resolution reduction.
        params->Set("DLSS.Feature.Create.Flags",(1<<1)|(1<<6));
        params->Set("DLSS.Enable.Output.Subrects",0);
        lastResult=SafeCreate(context,params,&handle);
        Log("DLSS create=%08X feature=1 mode=DLAA extent=%ux%u handle=%p",lastResult,w,h,handle);
        width=w;height=h;return lastResult==1&&handle;
    }
    bool Run(ID3D11DeviceContext* context,ID3D11Resource* color,ID3D11Resource* output,ID3D11Resource* depth,ID3D11Resource* mv,bool reset,float jitterX,float jitterY){
        if(!handle||!params||!color||!output||!depth||!mv)return false;
        params->Set("Color",color);params->Set("Output",output);params->Set("Depth",depth);params->Set("MotionVectors",mv);
        params->Set("Jitter.Offset.X",jitterX);params->Set("Jitter.Offset.Y",jitterY);
        params->Set("MV.Scale.X",1.f);params->Set("MV.Scale.Y",1.f);
        params->Set("Reset",reset?1:0);params->Set("DLSS.Render.Subrect.Dimensions.Width",width);params->Set("DLSS.Render.Subrect.Dimensions.Height",height);
        params->Set("DLSS.Pre.Exposure",1.f);params->Set("DLSS.Exposure.Scale",1.f);
        params->Set("Sharpness",0.f);params->Set("ToneMapperType",0);params->Set("FrameTimeDeltaInMsec",16.6667f);
        lastResult=SafeEvaluate(context,handle,params);
        if(lastResult!=1){Log("DLSS evaluate=%08X; output bypassed",lastResult);ReleaseFeature();return false;}return true;
    }
    void ReleaseFeature(){if(handle){Log("releasing DLSS feature");NVSDK_NGX_D3D11_ReleaseFeature(handle);Log("DLSS feature released");}handle=nullptr;}
    void Resize(){ReleaseFeature();attempted=false;}
    void Shutdown(ID3D11Device* device){
        ReleaseFeature();if(params){NVSDK_NGX_D3D11_DestroyParameters(params);params=nullptr;}
        if(initialized&&device){ComPtr<ID3D11DeviceContext> c;device->GetImmediateContext(&c);c->ClearState();c->Flush();Log("NGX shutdown after context flush");NVSDK_NGX_D3D11_Shutdown1(device);initialized=false;Log("NGX shutdown complete");}
    }
};
