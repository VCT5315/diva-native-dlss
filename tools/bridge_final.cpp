// Head-to-head: which sharing recipe actually moves data D3D11 -> D3D12 ->
// D3D11 with the exact access pattern the FSR4 backend uses (CopyResource in,
// CopyResource out, GPU fence handshake, no keyed-mutex acquire on the D3D12
// side because D3D12 cannot expose IDXGIKeyedMutex).
//
//   bridge_final.exe legacy     D3D11_RESOURCE_MISC_SHARED
//   bridge_final.exe keyed      SHARED_NTHANDLE | SHARED_KEYEDMUTEX
//   bridge_final.exe keyed-acq  the same, with AcquireSync/ReleaseSync on D3D11
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
using Microsoft::WRL::ComPtr;

static const UINT kW = 64, kH = 64, kRowPitch = kW * 4;

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* mode = argc > 1 ? argv[1] : "legacy";
    const bool useNtHandle = strcmp(mode, "legacy") != 0;
    const bool acquireMutex = strcmp(mode, "keyed-acq") == 0;
    printf("mode=%s (ntHandle=%d acquireMutex=%d)\n", mode, useNtHandle, acquireMutex);

    ComPtr<ID3D11Device> d11;
    ComPtr<ID3D11DeviceContext> c11;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &d11, nullptr, &c11))) return 1;
    ComPtr<IDXGIDevice> dg; d11.As(&dg);
    ComPtr<IDXGIAdapter> adapter; dg->GetAdapter(&adapter);
    ComPtr<ID3D12Device> d12;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12)))) return 2;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = kW; td.Height = kH; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    td.MiscFlags = useNtHandle ? (D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)
                               : D3D11_RESOURCE_MISC_SHARED;
    ComPtr<ID3D11Texture2D> shared;
    HRESULT hr = d11->CreateTexture2D(&td, nullptr, &shared);
    printf("create shared        hr=0x%08X\n", unsigned(hr));
    if (FAILED(hr)) return 3;

    HANDLE handle = nullptr;
    if (useNtHandle) { ComPtr<IDXGIResource1> r1; shared.As(&r1); hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle); }
    else             { ComPtr<IDXGIResource>  r0; shared.As(&r0); hr = r0->GetSharedHandle(&handle); }
    printf("share handle         hr=0x%08X\n", unsigned(hr));
    if (FAILED(hr)) return 4;

    ComPtr<ID3D12Resource> alias;
    hr = d12->OpenSharedHandle(handle, IID_PPV_ARGS(&alias));
    if (useNtHandle) CloseHandle(handle);
    printf("d3d12 open           hr=0x%08X\n", unsigned(hr));
    if (FAILED(hr)) return 5;
    printf("d3d12 flags          0x%X (ALLOW_UAV=%d)\n", alias->GetDesc().Flags,
           (alias->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ? 1 : 0);

    // --- D3D12 output surface the upscaler would write, shared back ---------
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 4;
    d12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    d12->CreateUnorderedAccessView(alias.Get(), nullptr, &uav, heap->GetCPUDescriptorHandleForHeapStart());
    printf("d3d12 UAV created    ok\n");

    // --- fences -------------------------------------------------------------
    ComPtr<ID3D11Device5> dev5; d11.As(&dev5);
    ComPtr<ID3D11Fence> fA11, fB11;
    dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fA11));
    dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fB11));
    ComPtr<ID3D12Fence> fA12, fB12;
    HANDLE h = nullptr;
    fA11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h); d12->OpenSharedHandle(h, IID_PPV_ARGS(&fA12)); CloseHandle(h);
    fB11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h); d12->OpenSharedHandle(h, IID_PPV_ARGS(&fB12)); CloseHandle(h);
    ComPtr<ID3D11DeviceContext4> c4; c11.As(&c4);

    // --- D3D12 readback -----------------------------------------------------
    D3D12_HEAP_PROPERTIES rh{}; rh.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = UINT64(kRowPitch) * kH; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    d12->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));

    ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    ComPtr<ID3D12CommandAllocator> alloc; d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    ComPtr<ID3D12GraphicsCommandList> list;
    d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list));

    // ======================= frame 1: D3D11 -> D3D12 =======================
    BYTE* pattern = static_cast<BYTE*>(malloc(size_t(kRowPitch) * kH));
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x)
        {
            BYTE* p = pattern + y * kRowPitch + x * 4;
            p[0] = BYTE(x * 4); p[1] = BYTE(y * 4); p[2] = 0x5A; p[3] = 0xFF;
        }

    ComPtr<IDXGIKeyedMutex> mutex; shared.As(&mutex);
    if (acquireMutex && mutex) { HRESULT m = mutex->AcquireSync(0, 1000); printf("d3d11 AcquireSync   hr=0x%08X\n", unsigned(m)); }

    D3D11_TEXTURE2D_DESC ud = td; ud.MiscFlags = 0;
    D3D11_SUBRESOURCE_DATA data{ pattern, kRowPitch, 0 };
    ComPtr<ID3D11Texture2D> upload;
    d11->CreateTexture2D(&ud, &data, &upload);
    c11->CopyResource(shared.Get(), upload.Get());

    if (acquireMutex && mutex) mutex->ReleaseSync(0);

    // Confirm the D3D11 write itself landed, with D3D12 already holding the alias.
    {
        D3D11_TEXTURE2D_DESC sd = td;
        sd.BindFlags = 0; sd.MiscFlags = 0; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        d11->CreateTexture2D(&sd, nullptr, &staging);
        c11->CopyResource(staging.Get(), shared.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(c11->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        {
            const BYTE* p = static_cast<const BYTE*>(mapped.pData);
            printf("d3d11 write check    got=%u,%u,%u,%u want=%u,%u,%u  %s\n",
                   p[0], p[1], p[2], p[3], pattern[0], pattern[1], pattern[2],
                   (p[0] == pattern[0] && p[1] == pattern[1] && p[2] == pattern[2]) ? "WRITE OK" : "WRITE LOST");
            c11->Unmap(staging.Get(), 0);
        }
    }

    c4->Signal(fA11.Get(), 1);
    c11->Flush();

    const bool useBarrier = GetEnvironmentVariableW(L"NO_BARRIER", nullptr, 0) == 0;
    printf("barriers             %s\n", useBarrier ? "explicit" : "none (rely on COMMON promotion)");

    queue->Wait(fA12.Get(), 1);
    if (useBarrier)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = alias.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b);
    }
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = alias.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = kW;
    dst.PlacedFootprint.Footprint.Height = kH;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = kRowPitch;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (useBarrier)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = alias.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        list->ResourceBarrier(1, &b);
    }
    list->Close();
    ID3D12CommandList* lists[] = { list.Get() };
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fB12.Get(), 1);

    ComPtr<ID3D12Fence> done; d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // Wait on value 1, not 0: a fresh fence already reports 0, so waiting on 0
    // would fire the event immediately and read the buffer before the GPU ran.
    done->SetEventOnCompletion(1, ev);
    queue->Signal(done.Get(), 1);
    if (WaitForSingleObject(ev, 5000) != WAIT_OBJECT_0) { printf("gpu timeout\n"); return 6; }
    CloseHandle(ev);

    const D3D12_RANGE range{ 0, UINT64(kRowPitch) * kH };
    void* mapped = nullptr;
    readback->Map(0, &range, &mapped);
    int mismatches = 0;
    for (UINT i = 0; i < kW * kH; ++i)
        if (memcmp(static_cast<const BYTE*>(mapped) + i * 4, pattern + i * 4, 3) != 0) ++mismatches;
    const BYTE* first = static_cast<const BYTE*>(mapped);
    printf("f1 d3d11->d3d12  first=%u,%u,%u,%u want=%u,%u,%u  mismatches=%d/%u\n",
           first[0], first[1], first[2], first[3], pattern[0], pattern[1], pattern[2], mismatches, kW * kH);
    readback->Unmap(0, nullptr);
    free(pattern);
    printf("\n%s\n", mismatches ? "FAILED" : "PASSED");
    return mismatches ? 1 : 0;
}
