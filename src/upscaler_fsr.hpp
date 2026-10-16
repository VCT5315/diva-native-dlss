#pragma once
#include "dx12_bridge.hpp"

// The FidelityFX API is a plain C surface exported by
// amd_fidelityfx_upscaler_dx12.dll (shipped by AMD inside FSR 4.1 titles).
// Headers are vendored from the FidelityFX SDK; the library is found at
// runtime, so the mod never needs it at link time.
#include "../third_party/ffx/upscalers/include/ffx_upscale.h"
#include "../third_party/ffx/api/include/dx12/ffx_api_dx12.h"

#include <fstream>
#include <sstream>
#include <winver.h>

// FSR needs a D3D12 command queue, so the whole backend is one interop bridge
// plus an FFX context. Inputs are identical to the DLSS path, which is why the
// two can be swapped frame by frame.
class FsrUpscaler : public IUpscaler
{
public:
    ~FsrUpscaler() override { Shutdown(); }

    const wchar_t* DisplayName() const override { return L"FSR 4.1"; }

    const wchar_t* StatusText() const override
    {
        if (libraryFailed) return L"upscaler DLL not found";
        if (!library)      return L"DLL not loaded";
        if (!context)      return L"FFX context not created";
        if (!contextReady) return statusDetail.c_str();
        return L"Native AA active";
    }

    bool Available() const override { return contextReady; }

    // -- DLL discovery --------------------------------------------------------

    static std::wstring Environment(const wchar_t* name)
    {
        wchar_t buffer[32768]{};
        const DWORD length = GetEnvironmentVariableW(name, buffer, ARRAYSIZE(buffer));
        return (length && length < ARRAYSIZE(buffer)) ? std::wstring(buffer, length) : std::wstring();
    }

    // Looks for amd_fidelityfx_upscaler_dx12.dll in the places AMD actually
    // puts it: next to the mod, next to the game, System32, and the Steam
    // libraries on this machine (many FSR4 titles ship a copy).
    static std::wstring FindUpscalerDll(const Settings& config)
    {
        const wchar_t* name = L"amd_fidelityfx_upscaler_dx12.dll";
        std::vector<std::wstring> candidates;

        if (!config.fsrDllPath.empty()) candidates.push_back(config.fsrDllPath);
        const std::wstring fromEnvironment = Environment(L"DIVA_FSR_DLL");
        if (!fromEnvironment.empty()) candidates.push_back(fromEnvironment);

        candidates.push_back(ModDirectory() + L"\\" + name);
        candidates.push_back(GameDirectory() + L"\\" + name);
        candidates.push_back(GameDirectory() + L"\\D3D12_Optiscaler\\" + name);
        if (const std::wstring windows = Environment(L"SystemRoot"); !windows.empty())
            candidates.push_back(windows + L"\\System32\\" + name);

        for (const auto& candidate : candidates)
        {
            if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) return candidate;
        }

        // Last resort: walk the Steam libraries a few levels deep. Bounded so a
        // big drive cannot stall the first frame. Several games ship a copy, so
        // pick the newest build rather than whichever the walk happens to hit
        // first - that is what makes FSR 4.1 win over an older 4.0 copy.
        std::vector<std::wstring> found;
        for (const auto& root : SteamCommonFolders())
        {
            SearchBounded(root,name,4,4000,found,8);
            if (found.size() >= 8) break;
        }
        if (!found.empty())
        {
            std::wstring best = found.front();
            DWORD bestScore = FileVersionScore(best);
            for (size_t i = 1; i < found.size(); ++i)
            {
                const DWORD score = FileVersionScore(found[i]);
                if (score > bestScore) { bestScore = score; best = found[i]; }
            }
            if (found.size() > 1) Log("FSR: %zu upscaler copies found, chose the newest (%u)", found.size(), bestScore);
            return best;
        }
        return {};
    }

    // major*10000 + minor*100 + patch, 0 when the version resource is missing.
    static DWORD FileVersionScore(const std::wstring& path)
    {
        DWORD handle = 0;
        const DWORD size = GetFileVersionInfoSizeW(path.c_str(),&handle);
        if (!size) return 0;
        std::vector<BYTE> buffer(size);
        if (!GetFileVersionInfoW(path.c_str(),0,size,buffer.data())) return 0;
        VS_FIXEDFILEINFO* info = nullptr;
        UINT length = 0;
        if (!VerQueryValueW(buffer.data(),L"\\",reinterpret_cast<void**>(&info),&length) || !info) return 0;
        const DWORD major = HIWORD(info->dwFileVersionMS);
        const DWORD minor = LOWORD(info->dwFileVersionMS);
        const DWORD patch = HIWORD(info->dwFileVersionLS);
        return major * 10000 + minor * 100 + patch;
    }

    // -- lifecycle ------------------------------------------------------------

    bool Initialize(ID3D11Device* d3d11,const Settings& config)
    {
        settings = &config;
        if (!bridge.Initialize(d3d11)) { statusDetail = L"D3D12 interop unavailable"; return false; }
        if (!LoadUpscalerLibrary(config)) { libraryFailed = true; return false; }
        return true;
    }

    bool Ready(const UpscaleFrame& frame) override
    {
        if (!library || !bridge.Valid() || !frame.width || !frame.height) return false;
        if (!settings) return false;
        lastHdr = frame.hdr;
        lastColorSpace = frame.colorSpace;

        if (context && contextWidth == frame.width && contextHeight == frame.height &&
            contextFlags == DesiredCreateFlags())
            return true;

        // Recreating the context releases shared surfaces the D3D12 queue may still be
        // reading. Changing the colour space or the resolution lands here, so this
        // path needs the same drain as teardown - without it those switches removed
        // the device.
        bridge.IdleWait();
        DestroyContext();
        depthRtv.Reset();   // the shared depth surface may be recreated below
        if (!bridge.EnsureSurfaces(frame.device,frame.width,frame.height,ColorFormat())) { statusDetail = L"shared surfaces failed"; return false; }
        EnsureDepthPipeline(frame.device);
        return CreateContext(frame.width,frame.height);
    }

    bool Run(const UpscaleFrame& frame) override
    {
        if (!context || !dispatch.Dispatch || !frame.context) return false;
        lastHdr = frame.hdr;
        lastColorSpace = frame.colorSpace;
        if (!bridge.EnsureSurfaces(frame.device,frame.width,frame.height,ColorFormat())) return false;

        ID3D11DeviceContext* c = frame.context;

        // 1. Claim this frame's token, wait until D3D12 is done with the
        //    previous frame's shared inputs, then publish colour/depth/motion.
        bridge.BeginFrame();
        bridge.WaitForInputsFree(c);
        c->CopyResource(bridge.Color(),frame.color);
        c->CopyResource(bridge.Motion(),frame.motion);
        UploadDepth(c,frame);
        bridge.SignalInputsReady(c);

        // 2. Run the upscaler on the D3D12 queue.
        ID3D12GraphicsCommandList* commandList = bridge.BeginDispatch();
        if (!commandList) { statusDetail = L"command list failed"; return false; }

        // The shared surfaces are written by D3D11 and are not tracked by the
        // D3D12 runtime, so COMMON is the state to declare: the upscaler
        // transitions out of it and back, which is the cross-API safe pattern.
        const uint32_t inputState = FFX_API_RESOURCE_STATE_COMMON;

        ffxDispatchDescUpscale desc{};
        desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        desc.commandList = commandList;
        desc.color       = ffxApiGetResourceDX12(bridge.Color12(),inputState);
        desc.depth       = ffxApiGetResourceDX12(bridge.Depth12(),inputState);
        desc.motionVectors = ffxApiGetResourceDX12(bridge.Motion12(),inputState);
        desc.output      = ffxApiGetResourceDX12(bridge.Output12(),inputState);
        desc.jitterOffset.x = frame.jitterX;
        desc.jitterOffset.y = frame.jitterY;
        const float motionScale = float(settings->fsrMvScalePercent) / 100.0f;
        desc.motionVectorScale.x = motionScale;
        desc.motionVectorScale.y = motionScale * float(settings->fsrMvSignY);
        desc.renderSize.width  = frame.width;
        desc.renderSize.height = frame.height;
        desc.upscaleSize.width = frame.width;   // Native AA: 1.0x, the DLAA analogue
        desc.upscaleSize.height = frame.height;
        desc.enableSharpening = settings->fsrSharpnessPercent > 0;
        desc.sharpness = float(settings->fsrSharpnessPercent) / 100.0f;
        desc.frameTimeDelta = frame.frameTimeMs;
        desc.preExposure = 1.0f;
        desc.reset = frame.reset;
        desc.cameraNear = 0.0f;
        desc.cameraFar = 0.0f;
        desc.cameraFovAngleVertical = 0.0f;
        desc.viewSpaceToMetersFactor = 0.0f;
        if (settings->fsrNonLinear) { /* legacy key, superseded by ColorSpace */ }
        switch (EffectiveColorSpace())
        {
        case 2:  desc.flags |= FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_PQ; break;
        case 1:  desc.flags |= FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB; break;
        default: break;   // linear scRGB: nothing to declare
        }
        if (settings->fsrDebugView) desc.flags |= FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;

        lastResult = dispatch.Dispatch(&context,&desc.header);
        if (lastResult != FFX_API_RETURN_OK)
        {
            Log("FSR dispatch=%u; output bypassed",lastResult);
            statusDetail = L"dispatch failed";
            // The list is still open; close and submit so the fence stays sane.
            bridge.EndDispatch();
            bridge.WaitForOutput(c);
            return false;
        }

        // 3. Submit, wait for it, and bring the result back to D3D11.
        if (!bridge.EndDispatch()) { statusDetail = L"submit failed"; return false; }
        bridge.WaitForOutput(c);
        c->CopyResource(frame.output,bridge.Output());

        ++frames;
        statusDetail = L"Native AA active";
        return true;
    }

    void Invalidate() override { bridge.IdleWait(); DestroyContext(); }

    void Shutdown() override
    {
        // Never destroy the context while a dispatch may still be queued.
        bridge.IdleWait();
        DestroyContext();
        bridge.Shutdown();
        if (library) { FreeLibrary(library); library = nullptr; }
        libraryFailed = false;
    }

    // Approximate VRAM the upscaler holds, for the configuration panel.
    unsigned long long GpuMemoryUsageBytes() const
    {
        if (!context || !dispatch.Query || !bridge.Device()) return 0;
        FfxApiEffectMemoryUsage usage{};
        ffxQueryDescUpscaleGetGPUMemoryUsage query{};
        query.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE;
        query.gpuMemoryUsageUpscaler = &usage;
        ffxContext local = context;
        if (dispatch.Query(&local,&query.header) != FFX_API_RETURN_OK) return 0;
        return usage.totalUsageInBytes;
    }

    const wchar_t* Detail() const { return statusDetail.c_str(); }

private:
    // FSR requires depth as single-channel FLOAT, and the game's depth is both
    // typeless and possibly D24/D16. So instead of a raw copy the shared depth
    // is filled by a one-triangle pass that samples the readable depth view and
    // writes R32_FLOAT. This also sidesteps typeless/typed copy rules entirely.
    bool EnsureDepthPipeline(ID3D11Device* device)
    {
        if (depthRtv) return true;
        if (!device) return false;

        static const char* code = R"(
Texture2D<float> Source : register(t0);
struct V { float4 p : SV_Position; };
V FullVS(uint id : SV_VertexID) { V v; v.p = float4(id==2?3:-1, id==1?3:-1, 0, 1); return v; }
float ConvertPS(V v) : SV_Target { return Source.Load(int3(v.p.xy, 0)); }
)";
        ComPtr<ID3DBlob> vs, ps, error;
        if (FAILED(D3DCompile(code,strlen(code),"FsrDepth",nullptr,nullptr,"FullVS","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&error)) ||
            FAILED(D3DCompile(code,strlen(code),"FsrDepth",nullptr,nullptr,"ConvertPS","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,&error)))
        {
            Log("FSR depth shader failed: %s",error?static_cast<char*>(error->GetBufferPointer()):"");
            return false;
        }
        if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&depthVS)) ||
            FAILED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&depthPS))) return false;

        D3D11_RASTERIZER_DESC rs{};
        rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE; rs.DepthClipEnable = TRUE;
        D3D11_DEPTH_STENCIL_DESC ds{}; ds.DepthEnable = FALSE;
        D3D11_BLEND_DESC bd{}; bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device->CreateRasterizerState(&rs,&fullscreenRaster)) ||
            FAILED(device->CreateDepthStencilState(&ds,&noDepth)) ||
            FAILED(device->CreateBlendState(&bd,&noBlend))) return false;

        if (FAILED(device->CreateRenderTargetView(bridge.Depth(),nullptr,&depthRtv)))
        { Log("FSR: R32_FLOAT depth RTV creation failed"); return false; }
        Log("FSR: depth conversion pipeline ready");
        return true;
    }

    void UploadDepth(ID3D11DeviceContext* c,const UpscaleFrame& frame)
    {
        if (!depthRtv || !frame.depthSrv || !depthVS || !depthPS)
        {
            // No readable view: fall back to a raw copy, which is only correct
            // when the source really is a 32-bit float format.
            c->CopyResource(bridge.Depth(),frame.depth);
            return;
        }
        ID3D11RenderTargetView* rtv = depthRtv.Get();
        c->OMSetRenderTargets(1,&rtv,nullptr);
        c->OMSetDepthStencilState(noDepth.Get(),0);
        float blendFactor[4]{}; c->OMSetBlendState(noBlend.Get(),blendFactor,0xffffffff);
        c->RSSetState(fullscreenRaster.Get());
        D3D11_VIEWPORT viewport{0,0,float(frame.width),float(frame.height),0,1};
        c->RSSetViewports(1,&viewport);
        c->IASetInputLayout(nullptr);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->VSSetShader(depthVS.Get(),nullptr,0);
        c->GSSetShader(nullptr,nullptr,0);
        c->HSSetShader(nullptr,nullptr,0);
        c->DSSetShader(nullptr,nullptr,0);
        c->PSSetShader(depthPS.Get(),nullptr,0);
        ID3D11ShaderResourceView* source = frame.depthSrv;
        c->PSSetShaderResources(0,1,&source);
        c->Draw(3,0);
        ID3D11ShaderResourceView* none = nullptr;
        c->PSSetShaderResources(0,1,&none);
        c->OMSetRenderTargets(0,nullptr,nullptr);
    }

    // Colour arrives either as a gamma-encoded 8-bit SDR frame or as an HDR
    // frame (HDR10 / scRGB). Both the working texture format and the flags the
    // upscaler is given have to match, or the image comes out washed out.
    // 1 sRGB (SDR) | 2 PQ (HDR10) | 3 linear (scRGB)
    int EffectiveColorSpace() const
    {
        if (lastColorSpace >= 1 && lastColorSpace <= 3) return lastColorSpace;
        return lastHdr ? 2 : 1;
    }

    DXGI_FORMAT ColorFormat() const
    {
        return lastHdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    // Motion vectors are at native resolution; only the transfer function and
    // the dynamic range depend on how the game presents.
    uint32_t DesiredCreateFlags() const
    {
        uint32_t flags = FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
        if (EffectiveColorSpace() != 3) flags |= FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
        if (lastHdr) flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
        if (settings && settings->fsrDebugView) flags |= FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION;
        return flags;
    }

    bool CreateContext(UINT width,UINT height)
    {
        createDesc = {};
        createDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        createDesc.flags = DesiredCreateFlags();
        createDesc.maxRenderSize.width = width;
        createDesc.maxRenderSize.height = height;
        createDesc.maxUpscaleSize.width = width;
        createDesc.maxUpscaleSize.height = height;
        createDesc.fpMessage = &FsrUpscaler::Message;

        backendDesc = {};
        backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backendDesc.device = bridge.Device();

        versionDesc = {};
        versionDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
        versionDesc.version = FFX_UPSCALER_VERSION;

        // Chain: upscale parameters -> D3D12 device -> API version.
        createDesc.header.pNext = &backendDesc.header;
        backendDesc.header.pNext = &versionDesc.header;
        versionDesc.header.pNext = nullptr;

        lastResult = dispatch.CreateContext(&context,&createDesc.header,nullptr);
        if (lastResult != FFX_API_RETURN_OK)
        {
            // A newer header than the installed DLL is the usual cause; retry
            // without the version tag so older libraries still work.
            Log("FSR createContext=%u with version %u; retrying without the version tag",lastResult,unsigned(FFX_UPSCALER_VERSION));
            createDesc.header.pNext = &backendDesc.header;
            backendDesc.header.pNext = nullptr;
            lastResult = dispatch.CreateContext(&context,&createDesc.header,nullptr);
        }
        if (lastResult != FFX_API_RETURN_OK || !context)
        {
            context = nullptr;
            statusDetail = L"FFX context creation failed";
            Log("FSR createContext failed=%u flags=%u extent=%ux%u",lastResult,createDesc.flags,width,height);
            return false;
        }

        contextWidth = width; contextHeight = height; contextFlags = createDesc.flags;
        contextReady = true;
        statusDetail = L"Native AA active";
        Log("FSR context created; version %u flags=%u extent=%ux%u",unsigned(FFX_UPSCALER_VERSION),createDesc.flags,width,height);
        return true;
    }

    void DestroyContext()
    {
        if (context && dispatch.DestroyContext)
        {
            ffxContext local = context;
            dispatch.DestroyContext(&local,nullptr);
        }
        context = nullptr;
        contextReady = false;
        contextWidth = contextHeight = 0;
        contextFlags = 0;
    }

    static void Message(uint32_t type,const wchar_t* message)
    {
        Log("FSR %s: %ls",type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning",message ? message : L"(null)");
    }

    bool LoadUpscalerLibrary(const Settings& config)
    {
        const std::wstring path = FindUpscalerDll(config);
        if (path.empty())
        {
            Log("FSR: amd_fidelityfx_upscaler_dx12.dll not found; FSR backend unavailable");
            return false;
        }
        // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR keeps the library's own siblings
        // resolvable when it is loaded out of another game's folder.
        HMODULE module = ::LoadLibraryExW(path.c_str(),nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) module = ::LoadLibraryW(path.c_str());
        if (!module) { Log("FSR: LoadLibrary failed=%lu path=%ls",GetLastError(),path.c_str()); return false; }

        dispatch.CreateContext  = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(module,"ffxCreateContext"));
        dispatch.DestroyContext = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(module,"ffxDestroyContext"));
        dispatch.Configure      = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(module,"ffxConfigure"));
        dispatch.Query          = reinterpret_cast<PfnFfxQuery>(GetProcAddress(module,"ffxQuery"));
        dispatch.Dispatch       = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(module,"ffxDispatch"));
        if (!dispatch.CreateContext || !dispatch.DestroyContext || !dispatch.Dispatch)
        {
            Log("FSR: %ls does not export the FidelityFX API",path.c_str());
            FreeLibrary(module);
            return false;
        }
        library = module; libraryPath = path;
        Log("FSR: loaded %ls",path.c_str());
        return true;
    }

    // Pulls library folders out of Steam's libraryfolders.vdf.
    static std::vector<std::wstring> SteamCommonFolders()
    {
        std::vector<std::wstring> folders;
        wchar_t steamPath[MAX_PATH]{};
        DWORD size = sizeof(steamPath);
        if (RegGetValueW(HKEY_CURRENT_USER,L"Software\\Valve\\Steam",L"SteamPath",RRF_RT_REG_SZ,nullptr,steamPath,&size) != ERROR_SUCCESS)
            return folders;

        std::vector<std::wstring> roots = { steamPath, steamPath + std::wstring(L"\\steamapps") };
        const std::wstring vdf = std::wstring(steamPath) + L"\\steamapps\\libraryfolders.vdf";
        std::wifstream file(vdf);
        if (file)
        {
            std::wstring line;
            while (std::getline(file,line))
            {
                const size_t key = line.find(L"\"path\"");
                if (key == std::wstring::npos) continue;
                const size_t first = line.find(L'"',key + 6);
                const size_t last = first == std::wstring::npos ? std::wstring::npos : line.find(L'"',first + 1);
                if (last == std::wstring::npos) continue;
                std::wstring path = line.substr(first + 1,last - first - 1);
                for (auto& ch : path) if (ch == L'/') ch = L'\\';
                roots.push_back(path + L"\\steamapps");
            }
        }

        for (const auto& root : roots) folders.push_back(root + L"\\common");
        return folders;
    }

    // Depth/breadth bounded directory walk; collects matches up to `cap`.
    static void SearchBounded(const std::wstring& root,const wchar_t* fileName,UINT maxDepth,size_t maxFolders,
                              std::vector<std::wstring>& out,size_t cap)
    {
        if (GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES) return;
        std::vector<std::pair<std::wstring,UINT>> queue = { { root,0 } };
        size_t visited = 0;
        while (!queue.empty() && visited < maxFolders && out.size() < cap)
        {
            auto [directory,depth] = queue.back();
            queue.pop_back();
            ++visited;

            const std::wstring direct = directory + L"\\" + fileName;
            if (GetFileAttributesW(direct.c_str()) != INVALID_FILE_ATTRIBUTES) out.push_back(direct);
            if (depth >= maxDepth) continue;

            WIN32_FIND_DATAW data{};
            HANDLE find = FindFirstFileW((directory + L"\\*").c_str(),&data);
            if (find == INVALID_HANDLE_VALUE) continue;
            do
            {
                if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (data.cFileName[0] == L'.') continue;
                queue.push_back({ directory + L"\\" + data.cFileName, depth + 1 });
            } while (FindNextFileW(find,&data));
            FindClose(find);
        }
    }

    struct DispatchFunctions
    {
        PfnFfxCreateContext  CreateContext  = nullptr;
        PfnFfxDestroyContext DestroyContext = nullptr;
        PfnFfxConfigure      Configure      = nullptr;
        PfnFfxQuery          Query          = nullptr;
        PfnFfxDispatch       Dispatch       = nullptr;
    };

    Dx12Bridge bridge;
    DispatchFunctions dispatch;
    HMODULE library = nullptr;
    std::wstring libraryPath;
    const Settings* settings = nullptr;

    // These descriptor structs must stay alive for the whole context lifetime,
    // so they are members rather than locals in CreateContext.
    ffxCreateContextDescUpscale createDesc{};
    ffxCreateBackendDX12Desc backendDesc{};
    ffxCreateContextDescUpscaleVersion versionDesc{};

    ffxContext context = nullptr;
    bool contextReady = false;
    bool libraryFailed = false;
    UINT contextWidth = 0,contextHeight = 0;
    uint32_t contextFlags = 0;
    bool lastHdr = false;
    int  lastColorSpace = 0;
    std::wstring statusDetail = L"not initialised";

    // Depth conversion pipeline (D24/D16/typeless -> R32_FLOAT).
    ComPtr<ID3D11VertexShader> depthVS;
    ComPtr<ID3D11PixelShader> depthPS;
    ComPtr<ID3D11RenderTargetView> depthRtv;
    ComPtr<ID3D11RasterizerState> fullscreenRaster;
    ComPtr<ID3D11DepthStencilState> noDepth;
    ComPtr<ID3D11BlendState> noBlend;
};
