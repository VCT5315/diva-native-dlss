// Drives the real FsrUpscaler from the mod against the real FSR 4.1 runtime
// that ships inside a game on this machine. This is the decisive check that the
// D3D11 -> D3D12 bridge plus the FidelityFX dispatch actually produce an image.
//
//   fsr_test.exe [path-to-amd_fidelityfx_upscaler_dx12.dll]
#include <windows.h>
#include <d3d11_4.h>
#include <string>
#include <cstdarg>
#include <cstdio>

// The mod's headers expect these from main.cpp.
static std::wstring g_modDirectory = L".";
std::wstring ModDirectory() { return g_modDirectory; }
std::wstring GameDirectory() { return g_modDirectory; }
void Log(const char* format, ...)
{
    va_list args; va_start(args, format);
    printf("      [log] "); vprintf(format, args); printf("\n");
    va_end(args);
}

#include "upscaler_fsr.hpp"

static const UINT kW = 1280, kH = 720;

static bool ReadBack(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                     unsigned char* out, UINT rowPitch)
{
    D3D11_TEXTURE2D_DESC sd{};
    texture->GetDesc(&sd);
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&sd, nullptr, &staging))) return false;
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    for (UINT y = 0; y < sd.Height; ++y)
        memcpy(out + y * rowPitch, static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch, rowPitch);
    context->Unmap(staging.Get(), 0);
    return true;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    const wchar_t* dll = argc > 1 ? nullptr : L"G:\\SteamLibrary\\steamapps\\common\\Battlefield 6\\amd_fidelityfx_upscaler_dx12.dll";
    const bool hdr = argc > 2 && strcmp(argv[2], "hdr") == 0;
    printf("mode: %s\n", hdr ? "HDR10 (R16G16B16A16_FLOAT working chain, PQ transfer)" : "SDR (R8G8B8A8_UNORM working chain)");
    std::wstring dllPath;
    if (argc > 1)
    {
        wchar_t wide[MAX_PATH]{};
        MultiByteToWideChar(CP_ACP, 0, argv[1], -1, wide, MAX_PATH);
        dllPath = wide;
    }
    else dllPath = dll;

    printf("upscaler DLL: %ls\n", dllPath.c_str());
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        printf("FAILED: DLL not found\n");
        return 2;
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &device, nullptr, &context)))
    {
        printf("FAILED: no D3D11 device\n");
        return 1;
    }

    // --- game-like textures -------------------------------------------------
    ComPtr<ID3D11Texture2D> color, depth, motion, output;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kW; td.Height = kH; td.MipLevels = 1; td.ArraySize = 1;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;

    unsigned char* pixels = static_cast<unsigned char*>(malloc(size_t(kW) * kH * 4));
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x)
        {
            unsigned char* p = pixels + (size_t(y) * kW + x) * 4;
            // Smooth gradient plus a checker so the upscaler has real detail.
            p[0] = (unsigned char)((x * 255) / kW);
            p[1] = (unsigned char)((y * 255) / kH);
            p[2] = (unsigned char)(((x / 16 + y / 16) & 1) ? 200 : 40);
            p[3] = 255;
        }

    td.Format = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{ pixels, kW * 4, 0 };
    if (FAILED(device->CreateTexture2D(&td, hdr ? nullptr : &data, &color))) { printf("FAILED: color\n"); return 3; }

    td.Format = DXGI_FORMAT_R32_FLOAT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    data.pSysMem = nullptr;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &depth))) { printf("FAILED: depth\n"); return 3; }
    // Plausible 0..1 depth so FSR is not fed garbage.
    {
        float* d = static_cast<float*>(malloc(size_t(kW) * kH * 4));
        for (UINT i = 0; i < kW * kH; ++i) d[i] = 0.1f + 0.8f * (float(i % kW) / kW);
        D3D11_SUBRESOURCE_DATA dd{ d, kW * 4, 0 };
        ComPtr<ID3D11Texture2D> staged;
        D3D11_TEXTURE2D_DESC sd = td; sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        device->CreateTexture2D(&sd, &dd, &staged);
        context->CopyResource(depth.Get(), staged.Get());
        free(d);
    }

    td.Format = DXGI_FORMAT_R16G16_FLOAT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &motion))) { printf("FAILED: motion\n"); return 3; }
    context->ClearRenderTargetView(nullptr, nullptr); // no-op; motion stays zero-initialised

    td.Format = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &output))) { printf("FAILED: output\n"); return 3; }
    // Give the upscaler something non-black to work with in HDR mode too.
    if (hdr)
    {
        std::vector<float> floats(size_t(kW) * kH * 4);
        for (UINT y = 0; y < kH; ++y)
            for (UINT x = 0; x < kW; ++x)
            {
                float* p = floats.data() + (size_t(y) * kW + x) * 4;
                p[0] = float(x) / kW; p[1] = float(y) / kH; p[2] = 0.5f; p[3] = 1.0f;
            }
        D3D11_TEXTURE2D_DESC cd = td; cd.BindFlags = D3D11_BIND_SHADER_RESOURCE; cd.MiscFlags = 0;
        D3D11_SUBRESOURCE_DATA fd{ floats.data(), kW * 16, 0 };
        ComPtr<ID3D11Texture2D> upload;
        device->CreateTexture2D(&cd, &fd, &upload);
        context->CopyResource(color.Get(), upload.Get());
    }

    ComPtr<ID3D11ShaderResourceView> depthSrv;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32_FLOAT; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(depth.Get(), &sd, &depthSrv);

    // --- the mod's backend --------------------------------------------------
    Settings settings;
    settings.fsrDllPath = dllPath;
    settings.fsrNonLinear = 1;          // the game backbuffer is gamma encoded
    settings.fsrSharpnessPercent = 0;
    settings.fsrMvScalePercent = 100;
    settings.fsrMvSignY = 1;

    FsrUpscaler fsr;
    const bool initialised = fsr.Initialize(device.Get(), settings);
    printf("Initialize           %s\n", initialised ? "ok" : "FAILED");

    UpscaleFrame frame;
    frame.device = device.Get();
    frame.context = context.Get();
    frame.color = color.Get();
    frame.output = output.Get();
    frame.depth = depth.Get();
    frame.depthSrv = depthSrv.Get();
    frame.depthFormat = DXGI_FORMAT_R32_FLOAT;
    frame.motion = motion.Get();
    frame.width = kW;
    frame.height = kH;
    frame.reset = true;
    frame.jitterX = 0.0f;
    frame.jitterY = 0.0f;
    frame.frameTimeMs = 16.6667f;
    frame.hdr = hdr;
    frame.colorSpace = hdr ? 2 : 0;   // 2 = PQ / HDR10

    const bool ready = fsr.Ready(frame);
    printf("Ready                %s (status=%ls)\n", ready ? "ok" : "FAILED", fsr.StatusText());
    if (!ready)
    {
        printf("\nFSR NOT AVAILABLE - lastResult=0x%08X\n", fsr.lastResult);
        return 4;
    }

    bool dispatched = false;
    for (int i = 0; i < 3; ++i)
    {
        frame.reset = (i == 0);
        dispatched = fsr.Run(frame);
        printf("Run #%d               %s (lastResult=0x%08X frames=%llu)\n",
               i + 1, dispatched ? "ok" : "FAILED", fsr.lastResult, fsr.frames);
        if (dispatched) break;
    }
    if (!dispatched) { printf("\nDISPATCH FAILED\n"); return 5; }

    // HDR output is 16-bit float. Rather than fight the staging readback, ask
    // the one question that matters: did the HDR dispatch complete cleanly, or
    // did it fault the GPU? A device-removed reason of S_OK means the whole
    // 16F chain (shared surfaces, FFX context, dispatch) was valid.
    if (hdr)
    {
        printf("\ndispatch returned OK; waiting for the GPU and checking for faults...\n");
        context->Flush();
        fsr.Shutdown();
        const HRESULT removed = device->GetDeviceRemovedReason();
        printf("device removed reason after HDR dispatch = 0x%08X\n", unsigned(removed));
        const bool clean = removed == S_OK;
        printf("\n%s\n", clean ? "FSR 4.1 HDR DISPATCH COMPLETED CLEANLY ON THE GPU"
                              : "FSR 4.1 HDR DISPATCH FAULTED THE DEVICE");
        return clean ? 0 : 7;
    }

    // --- inspect the result -------------------------------------------------
    unsigned char* result = static_cast<unsigned char*>(malloc(size_t(kW) * kH * 4));
    if (!ReadBack(device.Get(), context.Get(), output.Get(), result, kW * 4))
    {
        printf("FAILED: readback\n");
        return 6;
    }

    long long total = 0; int nonZero = 0; long long difference = 0;
    for (UINT i = 0; i < kW * kH; ++i)
    {
        const unsigned char* r = result + size_t(i) * 4;
        const unsigned char* s = pixels + size_t(i) * 4;
        total += r[0] + r[1] + r[2];
        if (r[0] || r[1] || r[2]) ++nonZero;
        difference += abs(int(r[0]) - int(s[0])) + abs(int(r[1]) - int(s[1])) + abs(int(r[2]) - int(s[2]));
    }
    const double mean = double(total) / double(size_t(kW) * kH * 3);
    const double mad = double(difference) / double(size_t(kW) * kH * 3);
    printf("\noutput non-black pixels %d / %u\n", nonZero, kW * kH);
    printf("output mean channel    %.1f (0=black, 127=mid)\n", mean);
    printf("mean abs diff vs input %.1f / 255\n", mad);
    printf("sample pixels          (0,0)=%u,%u,%u  (centre)=%u,%u,%u\n",
           result[0], result[1], result[2],
           result[(size_t(kH / 2) * kW + kW / 2) * 4], result[(size_t(kH / 2) * kW + kW / 2) * 4 + 1],
           result[(size_t(kH / 2) * kW + kW / 2) * 4 + 2]);

    const bool plausible = nonZero > int(kW * kH / 10) && mean > 5.0 && mean < 250.0;
    printf("\n%s\n", plausible ? "FSR 4.1 DISPATCH PRODUCED A PLAUSIBLE IMAGE"
                                : "FSR 4.1 DISPATCH PRODUCED A SUSPICIOUS IMAGE");
    fsr.Shutdown();
    return plausible ? 0 : 7;
}
