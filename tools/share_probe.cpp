// Which D3D11 sharing recipe survives the trip into D3D12, and does the
// opened resource keep the flags FSR needs (SRV for inputs, UAV for output)?
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
using Microsoft::WRL::ComPtr;

static int g_failures = 0;
static ComPtr<ID3D12InfoQueue> g_queue;
static UINT64 g_seen = 0;

static void Drain(const char* stage)
{
    if (!g_queue) return;
    const UINT64 count = g_queue->GetNumStoredMessages();
    for (UINT64 i = g_seen; i < count; ++i)
    {
        SIZE_T length = 0;
        if (FAILED(g_queue->GetMessage(i, nullptr, &length)) || !length) continue;
        D3D12_MESSAGE* m = static_cast<D3D12_MESSAGE*>(malloc(length));
        if (!m) continue;
        if (SUCCEEDED(g_queue->GetMessage(i, m, &length)) && m->pDescription)
        {
            if (m->Severity == D3D12_MESSAGE_SEVERITY_ERROR || m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
            {
                printf("      d3d12 ERROR [%s]: %s\n", stage, m->pDescription);
                ++g_failures;
            }
        }
        free(m);
    }
    g_seen = count;
}

struct Recipe
{
    const char* name;
    DXGI_FORMAT format;
    UINT bind;
    UINT misc;
    bool useLegacyHandle;   // GetSharedHandle vs CreateSharedHandle
    bool wantUav;
};

static void Run(ID3D11Device* d11, ID3D12Device* d12, ID3D12DescriptorHeap* heap, const Recipe& r)
{
    printf("\n== %s ==\n", r.name);

    D3D11_TEXTURE2D_DESC d{};
    d.Width = 640; d.Height = 360; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = r.format; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = r.bind; d.MiscFlags = r.misc;
    ComPtr<ID3D11Texture2D> tex;
    HRESULT hr = d11->CreateTexture2D(&d, nullptr, &tex);
    printf("   create            hr=0x%08X %s\n", unsigned(hr), SUCCEEDED(hr) ? "OK" : "FAILED");
    if (FAILED(hr)) { ++g_failures; return; }

    HANDLE handle = nullptr;
    if (r.useLegacyHandle)
    {
        ComPtr<IDXGIResource> resource;
        if (FAILED(tex.As(&resource))) { printf("   IDXGIResource     missing\n"); ++g_failures; return; }
        hr = resource->GetSharedHandle(&handle);
    }
    else
    {
        ComPtr<IDXGIResource1> resource;
        if (FAILED(tex.As(&resource))) { printf("   IDXGIResource1    missing\n"); ++g_failures; return; }
        hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
    }
    printf("   share handle      hr=0x%08X %s\n", unsigned(hr), SUCCEEDED(hr) ? "OK" : "FAILED");
    if (FAILED(hr)) { ++g_failures; return; }

    ComPtr<ID3D12Resource> opened;
    hr = d12->OpenSharedHandle(handle, IID_PPV_ARGS(&opened));
    if (r.useLegacyHandle) CloseHandle(handle);
    printf("   d3d12 open        hr=0x%08X %s\n", unsigned(hr), SUCCEEDED(hr) ? "OK" : "FAILED");
    if (FAILED(hr) || !opened) { ++g_failures; return; }

    const D3D12_RESOURCE_DESC rd = opened->GetDesc();
    printf("   d3d12 flags       0x%X (ALLOW_UAV=%d)\n", rd.Flags,
           (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ? 1 : 0);
    printf("   d3d12 format      %d\n", int(rd.Format));

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = r.format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
    d12->CreateShaderResourceView(opened.Get(), &srv, cpu);
    printf("   d3d12 SRV         recorded\n");
    Drain("after SRV");

    if (r.wantUav)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = r.format;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE uavCpu = cpu;
        uavCpu.ptr += d12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        d12->CreateUnorderedAccessView(opened.Get(), nullptr, &uav, uavCpu);
        printf("   d3d12 UAV         recorded\n");
        Drain("after UAV");
    }

    // Keyed mutex presence on the D3D12 side (informational).
    ComPtr<IDXGIKeyedMutex> mutex;
    printf("   d3d12 keyedmutex  %s\n", SUCCEEDED(opened.As(&mutex)) ? "available" : "not available");
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();

    ComPtr<ID3D11Device> d11;
    ComPtr<ID3D11DeviceContext> c11;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &d11, nullptr, &c11))) return 1;

    ComPtr<IDXGIDevice> dxgiDevice;
    d11.As(&dxgiDevice);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC ad{};
    adapter->GetDesc(&ad);
    printf("adapter: %ls\n", ad.Description);

    ComPtr<ID3D12Device> d12;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12)))) return 2;
    if (SUCCEEDED(d12.As(&g_queue))) g_queue->SetMuteDebugOutput(FALSE);

    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 8;
    d12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));

    const UINT SRV = D3D11_BIND_SHADER_RESOURCE;
    const UINT UAV = D3D11_BIND_UNORDERED_ACCESS;
    const UINT NTH = D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    const UINT KEYED = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    const UINT LEGACY = D3D11_RESOURCE_MISC_SHARED;

    // Inputs (colour / depth / motion vectors): only need SRV on the D3D12 side.
    Run(d11.Get(), d12.Get(), heap.Get(), { "input: legacy SHARED + SRV", DXGI_FORMAT_R8G8B8A8_UNORM, SRV, LEGACY, true, false });
    Run(d11.Get(), d12.Get(), heap.Get(), { "input: SHARED_NTHANDLE|KEYEDMUTEX + SRV", DXGI_FORMAT_R8G8B8A8_UNORM, SRV, NTH | KEYED, false, false });
    Run(d11.Get(), d12.Get(), heap.Get(), { "input: R16G16_FLOAT legacy SHARED + SRV", DXGI_FORMAT_R16G16_FLOAT, SRV, LEGACY, true, false });
    Run(d11.Get(), d12.Get(), heap.Get(), { "input: R32_FLOAT legacy SHARED + SRV", DXGI_FORMAT_R32_FLOAT, SRV, LEGACY, true, false });

    // Output: FSR needs a UAV, so this is the decisive case.
    Run(d11.Get(), d12.Get(), heap.Get(), { "output: legacy SHARED + SRV|UAV", DXGI_FORMAT_R8G8B8A8_UNORM, SRV | UAV, LEGACY, true, true });
    Run(d11.Get(), d12.Get(), heap.Get(), { "output: SHARED_NTHANDLE|KEYEDMUTEX + SRV|UAV", DXGI_FORMAT_R8G8B8A8_UNORM, SRV | UAV, NTH | KEYED, false, true });

    printf("\n%s (%d failure%s)\n", g_failures ? "PROBE FAILED" : "PROBE PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
