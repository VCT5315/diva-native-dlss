// Standalone probe for the D3D11 <-> D3D12 interop assumptions the FSR4
// backend depends on. Run it on the target machine; it prints PASS/FAIL for
// each assumption so the bridge design can be settled before the mod is built.
//
//   cl /nologo /EHsc /std:c++17 /W4 interop_probe.cpp /link d3d11.lib d3d12.lib dxgi.lib
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
using Microsoft::WRL::ComPtr;

static int g_failures = 0;
static ComPtr<ID3D12InfoQueue> g_infoQueue;
static UINT64 g_messagesSeen = 0;

static void Check(const char* name, bool ok, const char* detail = "")
{
    printf("[%s] %s %s\n", ok ? "PASS" : "FAIL", name, detail);
    if (!ok) ++g_failures;
}

// Prints any new D3D12 validation errors/warnings since the last call. This is
// what makes the probe conclusive: a silently rejected UAV or barrier shows up
// here even though the API itself returns void.
static void DrainValidation(const char* stage)
{
    if (!g_infoQueue) return;
    const UINT64 count = g_infoQueue->GetNumStoredMessages();
    for (UINT64 i = g_messagesSeen; i < count; ++i)
    {
        SIZE_T length = 0;
        if (FAILED(g_infoQueue->GetMessage(i, nullptr, &length)) || !length) continue;
        D3D12_MESSAGE* message = static_cast<D3D12_MESSAGE*>(malloc(length));
        if (!message) continue;
        if (SUCCEEDED(g_infoQueue->GetMessage(i, message, &length)))
        {
            const char* severity = message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ? "CORRUPTION"
                                 : message->Severity == D3D12_MESSAGE_SEVERITY_ERROR     ? "ERROR"
                                 : message->Severity == D3D12_MESSAGE_SEVERITY_WARNING   ? "WARNING"
                                                                                         : nullptr;
            if (severity && message->pDescription)
            {
                printf("     d3d12 %s [%s]: %s\n", severity, stage, message->pDescription);
                if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                    message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) ++g_failures;
            }
        }
        free(message);
    }
    g_messagesSeen = count;
}

static HRESULT CreateD3D12Uav(ID3D12Device* device, ID3D12Resource* resource, DXGI_FORMAT format,
                              ID3D12DescriptorHeap* heap)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(resource, nullptr, &uav, heap->GetCPUDescriptorHandleForHeapStart());
    return S_OK;
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();

    ComPtr<ID3D11Device> d11;
    ComPtr<ID3D11DeviceContext> c11;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &d11, nullptr, &c11);
    Check("D3D11 device", SUCCEEDED(hr));
    if (FAILED(hr)) return 1;

    ComPtr<IDXGIDevice> dxgiDevice;
    d11.As(&dxgiDevice);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC ad{};
    adapter->GetDesc(&ad);
    printf("     adapter: %ls\n", ad.Description);

    ComPtr<ID3D12Device> d12;
    hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12));
    Check("D3D12 device on the same adapter", SUCCEEDED(hr));
    if (FAILED(hr)) return 2;

    if (SUCCEEDED(d12.As(&g_infoQueue))) g_infoQueue->SetMuteDebugOutput(FALSE);

    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 8;
    d12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));

    // --- 1a. D3D11 shared texture with UAV: expected to be rejected --------
    {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = 640; d.Height = 360; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        ComPtr<ID3D11Texture2D> tex;
        hr = d11->CreateTexture2D(&d, nullptr, &tex);
        char buffer[96];
        sprintf_s(buffer, "(hr=0x%08X - sharing a UAV texture is not allowed)", unsigned(hr));
        printf("[INFO] D3D11 shared texture with UAV bind: %s\n",
               SUCCEEDED(hr) ? "allowed" : "rejected");
        if (SUCCEEDED(hr))
        {
            // If a driver ever allows this, the D3D12 side must still see the
            // UAV flag for FSR to be able to write the output.
            ComPtr<IDXGIResource1> res1;
            tex.As(&res1);
            HANDLE h = nullptr;
            if (res1 && SUCCEEDED(res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h)))
            {
                ComPtr<ID3D12Resource> r12;
                if (SUCCEEDED(d12->OpenSharedHandle(h, IID_PPV_ARGS(&r12))) && r12)
                {
                    Check("opened D3D11 texture kept ALLOW_UNORDERED_ACCESS",
                          (r12->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0, buffer);
                    CreateD3D12Uav(d12.Get(), r12.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, heap.Get());
                    DrainValidation("UAV-on-D3D11-shared-texture");
                }
                if (h) CloseHandle(h);
            }
        }
        else
        {
            printf("       -> the upscale OUTPUT must be a D3D12-native shared texture\n");
        }
    }

    // --- 1b. D3D11 shared texture used as an SRV input (what FSR needs) -----
    {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = 640; d.Height = 360; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        ComPtr<ID3D11Texture2D> tex;
        hr = d11->CreateTexture2D(&d, nullptr, &tex);
        Check("create D3D11 shared SRV-only texture (FSR input)", SUCCEEDED(hr));
        if (tex)
        {
            ComPtr<IDXGIResource1> res1;
            tex.As(&res1);
            HANDLE h = nullptr;
            hr = res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h);
            Check("CreateSharedHandle on the D3D11 input texture", SUCCEEDED(hr));
            ComPtr<ID3D12Resource> r12;
            if (SUCCEEDED(hr)) hr = d12->OpenSharedHandle(h, IID_PPV_ARGS(&r12));
            Check("D3D12 opens the D3D11 input texture", SUCCEEDED(hr) && r12 != nullptr);
            if (h) CloseHandle(h);
        }
    }

    // --- 2. Depth: typeless -> typed copy, and sharing it -------------------
    {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = 640; d.Height = 360; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R32_TYPELESS; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> typeless;
        hr = d11->CreateTexture2D(&d, nullptr, &typeless);
        d.Format = DXGI_FORMAT_R32_FLOAT;
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        ComPtr<ID3D11Texture2D> typed;
        hr = d11->CreateTexture2D(&d, nullptr, &typed);
        Check("create typeless + typed depth pair", typeless && typed);
        if (typeless && typed)
        {
            c11->CopyResource(typed.Get(), typeless.Get());
            c11->Flush();
            Check("D3D11 CopyResource R32_TYPELESS -> R32_FLOAT", true);
            ComPtr<IDXGIResource1> res1;
            typed.As(&res1);
            HANDLE h = nullptr;
            hr = res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h);
            ComPtr<ID3D12Resource> r12;
            if (SUCCEEDED(hr)) hr = d12->OpenSharedHandle(h, IID_PPV_ARGS(&r12));
            if (h) CloseHandle(h);
            Check("D3D12 opens the R32_FLOAT depth", SUCCEEDED(hr) && r12 != nullptr);
        }
    }

    // --- 3. Shared fences ---------------------------------------------------
    {
        ComPtr<ID3D11Device5> dev5;
        hr = d11.As(&dev5);
        Check("ID3D11Device5 available", SUCCEEDED(hr));

        ComPtr<ID3D11Fence> f11;
        if (dev5)
        {
            hr = dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&f11));
            Check("ID3D11Device5::CreateFence(SHARED)", SUCCEEDED(hr));
        }
        ComPtr<ID3D12Fence> f12;
        if (f11)
        {
            HANDLE h = nullptr;
            hr = f11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h);
            Check("ID3D11Fence::CreateSharedHandle", SUCCEEDED(hr));
            hr = d12->OpenSharedHandle(h, IID_PPV_ARGS(&f12));
            Check("D3D12 OpenSharedHandle on a D3D11 fence", SUCCEEDED(hr));
            if (h) CloseHandle(h);
        }

        ComPtr<ID3D11DeviceContext4> c4;
        c11.As(&c4);
        Check("ID3D11DeviceContext4 available", c4 != nullptr);
        if (c4 && f11 && f12)
        {
            c4->Signal(f11.Get(), 7);
            c4->Flush();
            char buffer[64]; sprintf_s(buffer, "(completed=%llu)", f12->GetCompletedValue());
            Check("fence value crosses D3D11 -> D3D12", f12->GetCompletedValue() == 7, buffer);
        }

        // Reverse direction: D3D12 fence opened by D3D11.
        ComPtr<ID3D12Fence> g12;
        if (SUCCEEDED(d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g12))))
        {
            HANDLE h = nullptr;
            hr = d12->CreateSharedHandle(g12.Get(), nullptr, GENERIC_ALL, nullptr, &h);
            Check("ID3D12Device::CreateSharedHandle(fence)", SUCCEEDED(hr));
            ComPtr<ID3D11Fence> g11;
            if (dev5 && SUCCEEDED(hr)) hr = dev5->OpenSharedFence(h, IID_PPV_ARGS(&g11));
            else hr = E_FAIL;
            Check("D3D11 OpenSharedFence on a D3D12 fence", SUCCEEDED(hr) && g11 != nullptr);
            if (h) CloseHandle(h);
        }
    }

    // --- 4. D3D12-native UAV texture shared back to D3D11 -------------------
    {
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = 640; rd.Height = 360; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> r12;
        hr = d12->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_SHARED, &rd,
                                          D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r12));
        Check("D3D12 committed resource with HEAP_FLAG_SHARED", SUCCEEDED(hr));
        if (r12)
        {
            HANDLE h = nullptr;
            hr = d12->CreateSharedHandle(r12.Get(), nullptr, GENERIC_ALL, nullptr, &h);
            Check("ID3D12Device::CreateSharedHandle(texture)", SUCCEEDED(hr));
            ComPtr<ID3D11Device1> dev1;
            d11.As(&dev1);
            ComPtr<ID3D11Texture2D> t11;
            hr = dev1->OpenSharedResource1(h, IID_PPV_ARGS(&t11));
            Check("D3D11 OpenSharedResource1 on a D3D12 UAV texture", SUCCEEDED(hr));
            if (h) CloseHandle(h);
            if (t11)
            {
                D3D11_TEXTURE2D_DESC td{};
                t11->GetDesc(&td);
                char buffer[128]; sprintf_s(buffer, "(bind=0x%X)", td.BindFlags);
                Check("D3D11 side sees the UAV bind flag",
                      (td.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0, buffer);
            }
        }
    }

    DrainValidation("overall");
    printf("\n%s (%d failure%s)\n", g_failures ? "PROBE FAILED" : "PROBE PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
