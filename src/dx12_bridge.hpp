#pragma once
#include "upscaler.hpp"
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>

// FSR4's upscaler library (amd_fidelityfx_upscaler_dx12.dll) is a D3D12 compute
// effect; it has no D3D11 backend. The game, however, owns a D3D11 immediate
// context that we cannot replace. So the two APIs are bridged:
//
//   * a second D3D12 device is created on the very same adapter;
//   * every shared surface is a D3D11 texture created with
//     D3D11_RESOURCE_MISC_SHARED_NTHANDLE and re-opened on the D3D12 side;
//   * two shared fences order the work, because neither API can wait on the
//     other's native synchronisation objects.
//
// Per frame the D3D11 context copies the game's colour/depth/motion vectors
// into the shared inputs, the D3D12 queue runs the upscaler into the shared
// output, and the D3D11 context copies it back out. Everything downstream in
// the renderer still sees plain D3D11 textures.
class Dx12Bridge
{
public:
    struct Shared
    {
        ComPtr<ID3D11Texture2D> tex11;
        ComPtr<ID3D12Resource>  res12;
        void Reset() { tex11.Reset(); res12.Reset(); }
        bool Ok() const { return tex11 && res12; }
    };

    bool Initialize(ID3D11Device* d3d11)
    {
        if (device12) return true;
        if (!d3d11) return false;

        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(d3d11->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) { Log("FSR bridge: IDXGIDevice unavailable"); return false; }
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDevice->GetAdapter(&adapter))) { Log("FSR bridge: adapter unavailable"); return false; }
        DXGI_ADAPTER_DESC adapterDesc{};
        adapter->GetDesc(&adapterDesc);
        Log("FSR bridge: adapter=%ls vendor=%04X device=%04X",adapterDesc.Description,adapterDesc.VendorId,adapterDesc.DeviceId);
        if (adapterDesc.VendorId != 0x1002)
            Log("FSR bridge: warning - vendor %04X is not AMD; FSR will most likely refuse to run",adapterDesc.VendorId);

        // Set DIVA_FSR_DEBUG=1 to turn on the D3D12 debug layer (slow, chatty,
        // but it is what makes interop validation failures visible).
        if (GetEnvironmentVariableW(L"DIVA_FSR_DEBUG",nullptr,0) > 0)
        {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) { debug->EnableDebugLayer(); Log("FSR bridge: D3D12 debug layer enabled"); }
        }

        // The D3D12 device has to sit on the same adapter as the game's D3D11
        // device, otherwise the shared handles cannot be opened.
        if (!CreateDevice(adapter.Get(),&device12)) return false;
        HRESULT hr = S_OK;

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = device12->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue));
        if (FAILED(hr)) { Log("FSR bridge: CreateCommandQueue failed=%08X",hr); return false; }
        hr = device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator));
        if (FAILED(hr)) { Log("FSR bridge: CreateCommandAllocator failed=%08X",hr); return false; }
        hr = device12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commandList));
        if (FAILED(hr)) { Log("FSR bridge: CreateCommandList failed=%08X",hr); return false; }
        commandList->Close();

        if (FAILED(d3d11->QueryInterface(IID_PPV_ARGS(&device5)))) { Log("FSR bridge: ID3D11Device5 unavailable"); return false; }
        if (!CreateFence(fenceA11,fenceA12,"A")) return false;   // D3D11 -> D3D12
        if (!CreateFence(fenceB11,fenceB12,"B")) return false;   // D3D12 -> D3D11

        Log("FSR bridge: D3D12 interop ready on the game's adapter");
        return true;
    }

    void Shutdown()
    {
        color.Reset(); depth.Reset(); motion.Reset(); output.Reset();
        commandList.Reset(); allocator.Reset(); queue.Reset();
        fenceA11.Reset(); fenceA12.Reset(); fenceB11.Reset(); fenceB12.Reset();
        device5.Reset(); device12.Reset();
        width = height = 0; frameIndex = 0;
    }

    bool Valid() const { return device12 && queue && commandList; }
    ID3D12Device* Device() const { return device12.Get(); }

    // (Re)creates the shared surfaces for an extent. Cheap once they exist.
    // `colorFormat` is the working format of the colour chain: R8G8B8A8_UNORM
    // for SDR output, R16G16B16A16_FLOAT when the game presents HDR.
    bool EnsureSurfaces(ID3D11Device* d3d11,UINT w,UINT h,DXGI_FORMAT colorFormat)
    {
        if (!Valid() || !d3d11) return false;
        if (color.Ok() && width == w && height == h && colorFormat == colorFmt) return true;
        color.Reset(); depth.Reset(); motion.Reset(); output.Reset();
        width = height = 0;

        // Colour in: the game's frame copy.
        if (!CreateShared(d3d11,color,w,h,colorFormat,D3D11_BIND_SHADER_RESOURCE)) return false;
        // Depth in: FSR wants a single-channel 32-bit float depth buffer. The
        // RTV bind flag lets the backend convert D24/D16 input into it.
        if (!CreateShared(d3d11,depth,w,h,DXGI_FORMAT_R32_FLOAT,
                          D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET)) return false;
        // Motion vectors in: 16-bit float pair at render resolution.
        if (!CreateShared(d3d11,motion,w,h,DXGI_FORMAT_R16G16_FLOAT,D3D11_BIND_SHADER_RESOURCE)) return false;
        // Colour out: FSR writes it as a UAV, then we copy it back to D3D11.
        if (!CreateShared(d3d11,output,w,h,colorFormat,
                          D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS)) return false;

        width = w; height = h; colorFmt = colorFormat;
        Log("FSR bridge: shared surfaces %ux%u fmt=%u created",w,h,unsigned(colorFormat));
        return true;
    }

    ID3D11Texture2D* Color()  const { return color.tex11.Get(); }
    ID3D11Texture2D* Depth()  const { return depth.tex11.Get(); }
    ID3D11Texture2D* Motion() const { return motion.tex11.Get(); }
    ID3D11Texture2D* Output() const { return output.tex11.Get(); }
    ID3D12Resource*  Color12()  const { return color.res12.Get(); }
    ID3D12Resource*  Depth12()  const { return depth.res12.Get(); }
    ID3D12Resource*  Motion12() const { return motion.res12.Get(); }
    ID3D12Resource*  Output12() const { return output.res12.Get(); }

    // --- per-frame handshake -------------------------------------------------
    // Order matters: BeginFrame -> WaitForInputsFree -> copy in ->
    // SignalInputsReady -> BeginDispatch -> dispatch -> EndDispatch ->
    // WaitForOutput -> copy out.
    //
    // frameIndex is the token for the frame being recorded. A D3D11 fence can
    // only be waited on for a value that some queued signal will actually
    // reach, so the input wait must target the PREVIOUS frame's token; waiting
    // for the current one would wait on a signal that is still behind the wait
    // itself and wedge the whole queue.
    void BeginFrame() { ++frameIndex; }

    // Blocks the D3D11 context until the D3D12 queue is done reading the shared
    // inputs from the previous frame.
    void WaitForInputsFree(ID3D11DeviceContext* context11)
    {
        auto* ctx4 = Context4(context11);
        if (ctx4 && fenceB11 && frameIndex > 1) ctx4->Wait(fenceB11.Get(),frameIndex - 1);
    }

    // Signals "the shared inputs for this frame are now written".
    void SignalInputsReady(ID3D11DeviceContext* context11)
    {
        auto* ctx4 = Context4(context11);
        if (ctx4 && fenceA11) ctx4->Signal(fenceA11.Get(),frameIndex);
    }

    // Starts a fresh command list that already waits for those copies.
    ID3D12GraphicsCommandList* BeginDispatch()
    {
        if (!Valid()) return nullptr;
        if (FAILED(allocator->Reset())) return nullptr;
        if (FAILED(commandList->Reset(allocator.Get(),nullptr))) return nullptr;
        if (fenceA12) queue->Wait(fenceA12.Get(),frameIndex);
        return commandList.Get();
    }

    // Closes the list, submits it, and signals the D3D12 -> D3D11 fence.
    bool EndDispatch()
    {
        if (!Valid()) return false;
        if (FAILED(commandList->Close())) return false;
        ID3D12CommandList* lists[] = { commandList.Get() };
        queue->ExecuteCommandLists(1,lists);
        if (fenceB12 && FAILED(queue->Signal(fenceB12.Get(),frameIndex))) return false;
        return true;
    }

    // Blocks the D3D11 context until the upscaled output is safe to read.
    // Blocks until the D3D12 queue has drained. Every step inside a frame waits on
    // the fences, but teardown did not - so switching FSR off destroyed the FFX
    // context and the shared surfaces while a dispatch could still be queued, and
    // releasing resources the GPU is reading removes the device. That is the crash
    // seen when turning FSR off.
    bool IdleWait()
    {
        if (!queue || !fenceB12) return false;
        const UINT64 target = ++frameIndex;
        if (FAILED(queue->Signal(fenceB12.Get(),target))) { Log("FSR bridge: idle signal failed"); return false; }
        if (fenceB12->GetCompletedValue() < target)
        {
            HANDLE done = CreateEventW(nullptr,FALSE,FALSE,nullptr);
            if (!done) return false;
            const HRESULT hr = fenceB12->SetEventOnCompletion(target,done);
            if (SUCCEEDED(hr)) WaitForSingleObject(done,2000);
            CloseHandle(done);
        }
        Log("FSR bridge: idle wait complete before teardown");
        return true;
    }
    void WaitForOutput(ID3D11DeviceContext* context11)
    {
        auto* ctx4 = Context4(context11);
        if (ctx4 && fenceB11) ctx4->Wait(fenceB11.Get(),frameIndex);
    }

    // Advances the timeline. Call once per fully consumed frame.
    UINT64 FrameIndex() const { return frameIndex; }

private:
    // A game process running ReShade with the Luma add-on is a problem here:
    // ReShade hooks d3d12.dll!D3D12CreateDevice and redirects it, Luma is
    // notified of the new device and aborts with "The application tried to
    // create a non DirectX 11 device", and the game dies.
    //
    // d3d12core.dll does NOT export D3D12CreateDevice at all - only
    // D3D12GetInterface and D3D12SDKVersion. The Agility SDK factory path
    // therefore builds the device without that hooked export ever being called,
    // so neither ReShade nor Luma notices it. Verified on this machine with
    // tools/dx12_factory_probe.cpp.
    static bool CreateDevice(IDXGIAdapter* adapter,ID3D12Device** device)
    {
        // d3d12.h declares these via DEFINE_GUID, which needs dxguid.lib; keep
        // local copies so the mod does not have to link it.
        static const GUID kClsidDeviceFactory = { 0x114863bf, 0xc386, 0x4aee, { 0xb3, 0x9d, 0x8f, 0x0b, 0xbb, 0x06, 0x29, 0x55 } };
        static const GUID kIidDeviceFactory   = { 0x61f307d3, 0xd34e, 0x4e7c, { 0x83, 0x74, 0x3b, 0xa4, 0xde, 0x23, 0xcc, 0xcb } };
        using PfnD3D12GetInterface = HRESULT(WINAPI*)(REFCLSID,REFIID,void**);

        // d3d12.dll must be asked first: d3d12core.dll is not re-exported until
        // the runtime has bootstrapped the factory, and returns E_NOINTERFACE
        // on a cold call (measured - see tools/dx12_factory_probe.cpp).
        HMODULE modules[2] = { LoadLibraryW(L"d3d12.dll"), LoadLibraryW(L"d3d12core.dll") };
        for (HMODULE module : modules)
        {
            if (!module) continue;
            auto getInterface = reinterpret_cast<PfnD3D12GetInterface>(GetProcAddress(module,"D3D12GetInterface"));
            if (!getInterface) continue;

            ComPtr<ID3D12DeviceFactory> factory;
            const HRESULT got = getInterface(kClsidDeviceFactory,kIidDeviceFactory,
                                             reinterpret_cast<void**>(factory.GetAddressOf()));
            if (FAILED(got) || !factory) { Log("FSR bridge: D3D12GetInterface failed=%08X",got); continue; }

            const HRESULT made = factory->CreateDevice(adapter,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(device));
            if (SUCCEEDED(made) && *device)
            {
                Log("FSR bridge: D3D12 device via ID3D12DeviceFactory (ReShade/Luma hooks bypassed)");
                return true;
            }
            Log("FSR bridge: ID3D12DeviceFactory::CreateDevice failed=%08X",made);
        }

        // No factory available (pre-1903 Windows). This is the export ReShade
        // redirects, so the Luma add-on will abort the game.
        const HRESULT hr = D3D12CreateDevice(adapter,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(device));
        Log("FSR bridge: fallback D3D12CreateDevice hr=%08X; ReShade/Luma will reject this",hr);
        return SUCCEEDED(hr) && *device;
    }

    ID3D11DeviceContext4* Context4(ID3D11DeviceContext* context11)
    {
        if (!context11) return nullptr;
        if (!context4 || cachedContext != context11)
        {
            context4.Reset();
            if (FAILED(context11->QueryInterface(IID_PPV_ARGS(&context4)))) { Log("FSR bridge: ID3D11DeviceContext4 unavailable"); context4.Reset(); }
            cachedContext = context11;
        }
        return context4.Get();
    }

    bool CreateShared(ID3D11Device* d3d11,Shared& out,UINT w,UINT h,DXGI_FORMAT format,UINT bindFlags)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w; desc.Height = h; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = format; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = bindFlags; desc.CPUAccessFlags = 0;
        // Legacy SHARED, not SHARED_NTHANDLE. Measured on the RX 9070 XT with
        // amdgpu driver 32.0.32015.2008 (tools/bridge_final.cpp):
        //   * SHARED_NTHANDLE alone is rejected with E_INVALIDARG; it is only
        //     accepted together with SHARED_KEYEDMUTEX.
        //   * With a keyed mutex, D3D11 writes are silently discarded unless
        //     the keyed mutex is acquired first, and IDXGIKeyedMutex cannot be
        //     used from the D3D12 side at all.
        //   * Legacy SHARED carries the data across with a plain CopyResource,
        //     keeps D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS (so the upscaler
        //     can write the output), and needs no mutex.
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        HRESULT hr = d3d11->CreateTexture2D(&desc,nullptr,&out.tex11);
        if (FAILED(hr)) { Log("FSR bridge: shared texture %ux%u fmt=%u failed=%08X",w,h,format,hr); return false; }

        ComPtr<IDXGIResource> dxgiResource;
        if (FAILED(out.tex11.As(&dxgiResource))) { Log("FSR bridge: IDXGIResource unavailable"); out.tex11.Reset(); return false; }
        HANDLE handle = nullptr;
        hr = dxgiResource->GetSharedHandle(&handle);
        if (FAILED(hr)) { Log("FSR bridge: GetSharedHandle failed=%08X",hr); out.tex11.Reset(); return false; }
        // A legacy shared handle is owned by the resource: do not CloseHandle it.
        hr = device12->OpenSharedHandle(handle,IID_PPV_ARGS(&out.res12));
        if (FAILED(hr)) { Log("FSR bridge: OpenSharedHandle failed=%08X",hr); out.Reset(); return false; }
        return true;
    }

    bool CreateFence(ComPtr<ID3D11Fence>& fence11,ComPtr<ID3D12Fence>& fence12,const char* label)
    {
        if (FAILED(device5->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence11))))
        { Log("FSR bridge: fence %s create failed",label); return false; }
        HANDLE handle = nullptr;
        HRESULT hr = fence11->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&handle);
        if (FAILED(hr)) { Log("FSR bridge: fence %s CreateSharedHandle failed=%08X",label,hr); return false; }
        // ID3D12Device::OpenSharedHandle covers shared resources, heaps and
        // fences; there is no dedicated OpenSharedFence entry point.
        hr = device12->OpenSharedHandle(handle,IID_PPV_ARGS(&fence12));
        CloseHandle(handle);
        if (FAILED(hr)) { Log("FSR bridge: fence %s OpenSharedHandle failed=%08X",label,hr); return false; }
        return true;
    }

    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext4> context4;
    ID3D11DeviceContext* cachedContext = nullptr;
    ComPtr<ID3D11Fence> fenceA11; // D3D11 -> D3D12
    ComPtr<ID3D12Fence> fenceA12;
    ComPtr<ID3D11Fence> fenceB11; // D3D12 -> D3D11
    ComPtr<ID3D12Fence> fenceB12;
    Shared color,depth,motion,output;
    UINT width = 0,height = 0,outWidth = 0,outHeight = 0;
    DXGI_FORMAT colorFmt = DXGI_FORMAT_UNKNOWN;
    UINT64 frameIndex = 0;
};
