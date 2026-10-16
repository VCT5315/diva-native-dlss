// Why does d3d12core!D3D12GetInterface(CLSID_D3D12DeviceFactory) return
// E_NOINTERFACE on the very first call, but succeed afterwards?
// This tries every ordering in a fresh process so the bridge can pick one that
// works on the FIRST attempt (a fallback to D3D12CreateDevice is what wakes up
// ReShade/Luma and kills the game).
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
using Microsoft::WRL::ComPtr;

static const GUID kClsidDeviceFactory = { 0x114863bf, 0xc386, 0x4aee, { 0xb3, 0x9d, 0x8f, 0x0b, 0xbb, 0x06, 0x29, 0x55 } };
static const GUID kIidDeviceFactory   = { 0x61f307d3, 0xd34e, 0x4e7c, { 0x83, 0x74, 0x3b, 0xa4, 0xde, 0x23, 0xcc, 0xcb } };
typedef HRESULT(WINAPI* PfnD3D12GetInterface)(REFCLSID, REFIID, void**);

static PfnD3D12GetInterface From(HMODULE module, const char* name)
{
    HMODULE m = module ? module : LoadLibraryW(L"d3d12.dll");
    auto fn = reinterpret_cast<PfnD3D12GetInterface>(GetProcAddress(m, "D3D12GetInterface"));
    printf("   %-28s module=%p D3D12GetInterface=%p\n", name, m, reinterpret_cast<void*>(fn));
    return fn;
}

static void Try(const char* label, PfnD3D12GetInterface fn)
{
    if (!fn) { printf("   %-28s <no export>\n", label); return; }
    ComPtr<ID3D12DeviceFactory> factory;
    const HRESULT hr = fn(kClsidDeviceFactory, kIidDeviceFactory, reinterpret_cast<void**>(factory.GetAddressOf()));
    printf("   %-28s hr=0x%08X factory=%p\n", label, unsigned(hr), factory.Get());
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("fresh process\n");

    HMODULE core = LoadLibraryW(L"d3d12core.dll");
    printf("   d3d12core.dll = %p\n", core);

    auto fromCore = From(core, "d3d12core!");
    auto fromDll  = From(nullptr, "d3d12.dll!");

    printf("\n-- ordering matrix --\n");
    Try("1) core first (fresh)", fromCore);
    Try("2) d3d12.dll after that", fromDll);
    Try("3) core again", fromCore);
    Try("4) core third time", fromCore);

    // Does the factory actually build a device, and is that independent of any
    // D3D12CreateDevice call?
    if (fromDll)
    {
        ComPtr<ID3D12DeviceFactory> factory;
        if (SUCCEEDED(fromDll(kClsidDeviceFactory, kIidDeviceFactory, reinterpret_cast<void**>(factory.GetAddressOf()))) && factory)
        {
            ComPtr<IDXGIFactory1> dxgi;
            CreateDXGIFactory1(IID_PPV_ARGS(&dxgi));
            ComPtr<IDXGIAdapter> adapter;
            dxgi->EnumAdapters(0, &adapter);
            ComPtr<ID3D12Device> device;
            const HRESULT hr = factory->CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
            printf("\n   d3d12.dll factory CreateDevice   hr=0x%08X device=%p\n", unsigned(hr), device.Get());
            printf("   adapter = %p (0x1414 Microsoft Basic / real GPU)\n", adapter.Get());
        }
    }
    return 0;
}
