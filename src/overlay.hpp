#pragma once
#include "upscaler.hpp"
#include <atomic>

// In-game configuration panel, opened with a hotkey (F7 by default).
//
// The panel is drawn with GDI into a DIB and uploaded as one texture, which is
// how the existing status toast already works. That keeps full typography and
// CJK support without pulling in a UI library, and the whole panel costs a
// single textured triangle per frame.
class ConfigOverlay
{
public:
    struct Stats
    {
        const wchar_t* backendName = L"-";
        const wchar_t* backendStatus = L"-";
        const wchar_t* detail = L"";
        double fps = 0.0;
        double frameMs = 0.0;
        unsigned long long upscaledFrames = 0;
        unsigned int nativeDraws = 0;
        unsigned int motionDraws = 0;
        unsigned int uiDraws = 0;
        unsigned long long historyBytes = 0;
        unsigned int width = 0;
        unsigned int height = 0;
        bool hdr = false;
        unsigned int lastResult = 0;
    };

    bool Initialize(ID3D11Device* device)
    {
        if (initialized) return true;
        if (!device) return false;

        // Deliberately mirrors the status toast's shader: SV_Position only,
        // integer Load from a constant-buffer origin, no UV interpolation and no
        // sampler. The toast is proven to appear on the game's backbuffer, so
        // the panel uses byte-for-byte the same technique.
        static const char* code = R"(
Texture2D<float4> Source : register(t0);
cbuffer Panel : register(b0) { float2 Origin; float2 Padding; };
struct V { float4 p : SV_Position; };
V FullVS(uint id : SV_VertexID) { V v; v.p = float4(id==2?3:-1, id==1?3:-1, 0, 1); return v; }
float4 TexturedPS(V v) : SV_Target { int2 p = int2(v.p.xy) - int2(Origin); return float4(Source.Load(int3(p, 0)).rgb, 1.0); }
)";
        ComPtr<ID3DBlob> vs, ps, error;
        if (FAILED(D3DCompile(code,strlen(code),"DivaOverlay",nullptr,nullptr,"FullVS","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&error)) ||
            FAILED(D3DCompile(code,strlen(code),"DivaOverlay",nullptr,nullptr,"TexturedPS","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,&error)))
        {
            Log("overlay shader failed: %s",error?static_cast<char*>(error->GetBufferPointer()):"");
            return false;
        }
        if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertexShader)) ||
            FAILED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixelShader))) return false;

        D3D11_SAMPLER_DESC sm{};
        sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sm.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device->CreateSamplerState(&sm,&sampler))) return false;

        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = 16; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device->CreateBuffer(&cb,nullptr,&constants))) return false;

        D3D11_RASTERIZER_DESC rs{};
        rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE; rs.DepthClipEnable = TRUE;
        D3D11_DEPTH_STENCIL_DESC ds{}; ds.DepthEnable = FALSE;
        D3D11_BLEND_DESC bd{};
        // Exactly the toast's "noBlend": write mask only, blending disabled.
        // A NULL blend state is NOT equivalent here - the toast uses a real
        // state and is the only version proven to appear on the game backbuffer.
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        bd.RenderTarget[0].BlendEnable = FALSE;
        bd.RenderTarget[0].BlendOp = bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        if (FAILED(device->CreateRasterizerState(&rs,&raster)) ||
            FAILED(device->CreateDepthStencilState(&ds,&depth)) ||
            FAILED(device->CreateBlendState(&bd,&blend))) return false;

        deviceRef = device;
        initialized = true;
        Log("overlay ready; %d rows",int(Rows().size()));
        return true;
    }

    bool Visible() const { return visible; }
    void Toggle() { visible = !visible; if (visible) { rebuild = true; probeLeft = 3; } }
    void SetVisible(bool value) { if (visible != value) { visible = value; if (visible) { rebuild = true; probeLeft = 3; } } }

    // --- input --------------------------------------------------------------
    // Returns true when the key was consumed and must not reach the game.
    bool OnKeyDown(UINT key,Settings& settings)
    {
        if (!visible) return false;
        const auto& rows = Rows();
        switch (key)
        {
        case VK_ESCAPE:
        case VK_F7:
            SetVisible(false); return true;
        case VK_UP:   selection = (selection + int(rows.size()) - 1) % int(rows.size()); rebuild = true; return true;
        case VK_DOWN: selection = (selection + 1) % int(rows.size()); rebuild = true; return true;
        case VK_LEFT:  Adjust(settings,-1); return true;
        case VK_RIGHT: Adjust(settings, 1); return true;
        case VK_RETURN: Adjust(settings, 1); return true;
        default: return false;
        }
    }

    void OnMouseMove(int x,int y)
    {
        if (!visible) return;
        mouseX = x; mouseY = y;
        const int row = RowAt(y);
        if (row >= 0 && row != selection) { selection = row; rebuild = true; }
    }

    void OnMouseButton(int x,int y)
    {
        if (!visible) return;
        mouseX = x; mouseY = y;
        const int row = RowAt(y);
        if (row >= 0)
        {
            selection = row;
            pendingClick = true;
            rebuild = true;
        }
    }

    // --- rendering ----------------------------------------------------------
    // `call` replays the original Draw so this does not recurse into the hooks.
    void Render(ID3D11DeviceContext* context,IDXGISwapChain* swap,Settings& settings,
                const Stats& stats,void(*call)(ID3D11DeviceContext*,bool,UINT,UINT,INT))
    {
        if (!visible || !initialized || !context || !swap) return;

        // Take the target first: its real size decides the layout, and it is the
        // only trustworthy source. A cached extent can disagree with what
        // GetBuffer(0) actually hands back (a proxy such as ReShade may wrap the
        // swap chain), which would place the panel off-target.
        ComPtr<ID3D11Texture2D> back;
        if (FAILED(swap->GetBuffer(0,IID_PPV_ARGS(&back))) || !back)
        {
            Log("overlay: GetBuffer(0) failed");
            return;
        }
        D3D11_TEXTURE2D_DESC bd{};
        back->GetDesc(&bd);
        if (bd.Width != screenWidth || bd.Height != screenHeight)
        {
            if (screenWidth || screenHeight)
                Log("overlay: target is %ux%u but cached extent was %ux%u; using the target",
                    bd.Width,bd.Height,screenWidth,screenHeight);
            screenWidth = bd.Width; screenHeight = bd.Height; rebuild = true;
        }

        if (!UpdateAnimation(settings,stats)) return;

        static std::atomic<unsigned int> announced{0};
        if (announced.fetch_add(1) < 4)
            Log("overlay drawing: panel=%d,%d %ux%u target=%ux%u fmt=%u texture=%d",
                panelX,panelY,width,height,bd.Width,bd.Height,unsigned(bd.Format),texture?1:0);

        // Take the target again immediately before binding it, exactly the way
        // RenderBadge does. Everything between the first GetBuffer and here
        // (GDI, GetImmediateContext, UpdateSubresource) is skipped.
        ComPtr<ID3D11Texture2D> targetTexture;
        if (FAILED(swap->GetBuffer(0,IID_PPV_ARGS(&targetTexture))) || !targetTexture) return;
        ComPtr<ID3D11RenderTargetView> target;
        if (FAILED(deviceRef->CreateRenderTargetView(targetTexture.Get(),nullptr,&target)) || !target)
        {
            Log("overlay: CreateRenderTargetView failed (format=%u %ux%u)",unsigned(bd.Format),bd.Width,bd.Height);
            return;
        }
        back = targetTexture;   // probe the same surface we drew into

        context->OMSetRenderTargets(1,&target,nullptr);
        context->OMSetDepthStencilState(depth.Get(),0);
        // The panel is opaque, so blending is not needed at all. Writing
        // straight through removes any dependence on the source alpha channel,
        // which GDI does not manage.
        float blendFactor[4]{};
        context->OMSetBlendState(blend.Get(),blendFactor,0xffffffff);
        context->RSSetState(raster.Get());
        D3D11_VIEWPORT viewport{float(panelX),float(panelY),float(width),float(height),0,1};
        context->RSSetViewports(1,&viewport);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertexShader.Get(),nullptr,0);
        context->GSSetShader(nullptr,nullptr,0);
        context->HSSetShader(nullptr,nullptr,0);
        context->DSSetShader(nullptr,nullptr,0);
        context->PSSetShader(pixelShader.Get(),nullptr,0);
        const float origin[4] = { float(panelX), float(panelY), 0.0f, 0.0f };
        context->UpdateSubresource(constants.Get(),0,nullptr,origin,0,0);
        ID3D11Buffer* cbs[] = { constants.Get() };
        context->PSSetConstantBuffers(0,1,cbs);
        ID3D11ShaderResourceView* view = textureSrv.Get();
        context->PSSetShaderResources(0,1,&view);
        // DIVA_OVERLAY_CLEARTEST=1 swaps the panel draw for the simplest
        // possible write, to separate "the draw is wrong" from "the target is
        // wrong" without shipping a broken panel.
        if (GetEnvironmentVariableW(L"DIVA_OVERLAY_CLEARTEST",nullptr,0) > 0)
        {
            const float magenta[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
            context->ClearRenderTargetView(target.Get(),magenta);
        }
        else call(context,false,3,0,0);

        // Did the pixels actually land in the target? Reading four of them back
        // right now separates "the draw went nowhere" from "something replaced
        // the frame before it was presented".
        if (probeLeft > 0)
        {
            --probeLeft;
            ProbeTarget(context,back.Get(),bd,
                        UINT(panelX) + UINT(width / 2), UINT(panelY) + UINT(height / 2));
        }

        ID3D11ShaderResourceView* none = nullptr;
        context->PSSetShaderResources(0,1,&none);
        context->OMSetRenderTargets(0,nullptr,nullptr);
    }

    // Maps a window-space pixel to panel space. Returns false if outside.
    void SetScreenSize(unsigned int w,unsigned int h) { screenWidth = w; screenHeight = h; }

private:
    enum class Kind { Enum, Toggle, Integer, Action, Info };
    enum class Id
    {
        Backend, BackendStatus, Sharpness, NonLinear, ColorSpace, MvScale, MvSignY, FsrDebugView,
        MotionDebug, StatusToast, HistoryMB, Trace, Capture,
        ActionReset, ActionReload, ActionSave,
        InfoFps, InfoResolution, InfoResult,
    };
    struct Row { const wchar_t* label; Kind kind; Id id; };

    static const std::vector<Row>& Rows()
    {
        static const std::vector<Row> rows = {
            { L"分离器 Upscaler",     Kind::Enum,    Id::Backend },
            { L"状态 Status",         Kind::Info,    Id::BackendStatus },
            { L"锐化 Sharpness",      Kind::Integer, Id::Sharpness },
            { L"非线性色彩 Non-linear",Kind::Toggle,  Id::NonLinear },
            { L"色彩空间 Color space",  Kind::Enum,    Id::ColorSpace },
            { L"运动矢量缩放 MV scale",Kind::Integer, Id::MvScale },
            { L"运动矢量 Y 方向",      Kind::Toggle,  Id::MvSignY },
            { L"FSR 调试视图",        Kind::Toggle,  Id::FsrDebugView },
            { L"运动矢量预览 (F5)",    Kind::Toggle,  Id::MotionDebug },
            { L"开关提示 toast",       Kind::Toggle,  Id::StatusToast },
            { L"几何历史 History MB",  Kind::Integer, Id::HistoryMB },
            { L"仅跟踪 TraceOnly",     Kind::Toggle,  Id::Trace },
            { L"仅捕获 CaptureOnly",   Kind::Toggle,  Id::Capture },
            { L"重置时间历史",         Kind::Action,  Id::ActionReset },
            { L"重新载入 ini",         Kind::Action,  Id::ActionReload },
            { L"保存 ini",             Kind::Action,  Id::ActionSave },
            { L"帧率 FPS",             Kind::Info,    Id::InfoFps },
            { L"分辨率",               Kind::Info,    Id::InfoResolution },
            { L"后端返回码",           Kind::Info,    Id::InfoResult },
        };
        return rows;
    }

    void Adjust(Settings& settings,int direction)
    {
        const auto& rows = Rows();
        if (selection < 0 || selection >= int(rows.size())) return;
        const Id id = rows[selection].id;
        bool changed = true;
        switch (id)
        {
        case Id::Backend: {
            int value = (settings.backend + direction + 4) % 4;
            settings.backend = value;
            settings.Save();
            break;
        }
        case Id::Sharpness:
            settings.fsrSharpnessPercent = std::clamp(settings.fsrSharpnessPercent + direction * 5,0,100);
            settings.Save();
            break;
        case Id::NonLinear:
            settings.fsrNonLinear = !settings.fsrNonLinear;
            settings.Save();
            break;
        case Id::ColorSpace: {
            settings.fsrColorSpace = (settings.fsrColorSpace + direction + 4) % 4;
            settings.Save();
            break;
        }
        case Id::MvScale:
            settings.fsrMvScalePercent = std::clamp(settings.fsrMvScalePercent + direction * 5,25,400);
            settings.Save();
            break;
        case Id::MvSignY:
            settings.fsrMvSignY = -settings.fsrMvSignY;
            settings.Save();
            break;
        case Id::FsrDebugView:
            settings.fsrDebugView = !settings.fsrDebugView;
            settings.Save();
            break;
        case Id::MotionDebug:
            settings.debugMotion = !settings.debugMotion;
            settings.Save();
            break;
        case Id::StatusToast:
            settings.showStatus = !settings.showStatus;
            settings.Save();
            break;
        case Id::HistoryMB:
            settings.historyMB = std::clamp(settings.historyMB + direction * 32,32,512);
            settings.Save();
            break;
        case Id::Trace:
            settings.traceOnly = !settings.traceOnly;
            settings.Save();
            break;
        case Id::Capture:
            settings.captureOnly = !settings.captureOnly;
            settings.Save();
            break;
        case Id::ActionReset:
            request.resetHistory = true;
            SaveAndReload();
            break;
        case Id::ActionReload:
            request.reloadSettings = true;
            SaveAndReload();
            break;
        case Id::ActionSave:
            settings.Save();
            SaveAndReload();
            break;
        default:
            changed = false;
            break;
        }
        if (changed) { rebuild = true; flashUntil = GetTickCount64() + 400; }
        pendingClick = false;
    }

    void SaveAndReload() { request.reloadSettings = true; }

    int RowAt(int y) const
    {
        const int relative = y - (panelY + headerHeight + padding);
        if (relative < 0) return -1;
        const int index = relative / rowHeight;
        if (index < 0 || index >= int(Rows().size())) return -1;
        return index;
    }

    std::wstring ValueText(const Settings& settings,const Stats& stats,Id id) const
    {
        wchar_t buffer[128]{};
        switch (id)
        {
        case Id::Backend:       return BackendName(Backend(settings.backend));
        case Id::BackendStatus: return stats.backendStatus ? stats.backendStatus : L"-";
        case Id::Sharpness:     swprintf_s(buffer,L"%d %%",settings.fsrSharpnessPercent); return buffer;
        case Id::NonLinear:     return settings.fsrNonLinear ? L"开" : L"关";
        case Id::ColorSpace:    return Settings::ColorSpaceName(settings.fsrColorSpace);
        case Id::MvScale:       swprintf_s(buffer,L"%d %%",settings.fsrMvScalePercent); return buffer;
        case Id::MvSignY:       return settings.fsrMvSignY > 0 ? L"+1 (下正)" : L"-1 (上正)";
        case Id::FsrDebugView:  return settings.fsrDebugView ? L"开" : L"关";
        case Id::MotionDebug:   return settings.debugMotion ? L"开" : L"关";
        case Id::StatusToast:   return settings.showStatus ? L"开" : L"关";
        case Id::HistoryMB:     swprintf_s(buffer,L"%d MB",settings.historyMB); return buffer;
        case Id::Trace:         return settings.traceOnly ? L"开" : L"关";
        case Id::Capture:       return settings.captureOnly ? L"开" : L"关";
        case Id::ActionReset:   return L"执行";
        case Id::ActionReload:  return L"读取";
        case Id::ActionSave:    return L"写入";
        case Id::InfoFps:       swprintf_s(buffer,L"%.1f (%.2f ms)",stats.fps,stats.frameMs); return buffer;
        case Id::InfoResolution:swprintf_s(buffer,L"%ux%u %ls",stats.width,stats.height,stats.hdr?L"HDR":L"SDR"); return buffer;
        case Id::InfoResult:    swprintf_s(buffer,L"0x%08X  已处理 %llu",stats.lastResult,stats.upscaledFrames); return buffer;
        default:                return L"";
        }
    }

    // Rebuilds the DIB only when something visible actually changed; the FPS row
    // is refreshed a few times per second so the panel is not constantly uploading.
    bool UpdateAnimation(const Settings& settings,const Stats& stats)
    {
        const UINT64 now = GetTickCount64();
        const bool statsDue = now - lastStatsSample > 250;
        if (rebuild || statsDue || !texture)
        {
            lastStatsSample = now;
            rebuild = false;
            return Draw(settings,stats);
        }
        return true;
    }

    // Copies a few texels back out of the render target right after the panel
    // draw and logs them. If they read as panel colours, the draw landed and
    // anything unseen means the frame was replaced later; if they read as the
    // scene, the draw never reached this surface at all.
    void ProbeTarget(ID3D11DeviceContext* context,ID3D11Texture2D* back,const D3D11_TEXTURE2D_DESC& bd,UINT x,UINT y)
    {
        if (x + 4 > bd.Width || y + 4 > bd.Height) { Log("overlay probe: sample point outside target"); return; }
        D3D11_TEXTURE2D_DESC sd = bd;
        sd.Width = 4; sd.Height = 4; sd.MipLevels = 1; sd.ArraySize = 1;
        sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(deviceRef->CreateTexture2D(&sd,nullptr,&staging))) { Log("overlay probe: staging create failed"); return; }
        const D3D11_BOX box{ x, y, 0, x + 4, y + 4, 1 };
        context->CopySubresourceRegion(staging.Get(),0,0,0,0,back,0,&box);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))) { Log("overlay probe: map failed"); return; }
        if (bd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || bd.Format == DXGI_FORMAT_R32G32B32A32_FLOAT)
            Log("overlay probe @%u,%u = %.3f %.3f %.3f %.3f  (16F target)",x,y,
                static_cast<const float*>(mapped.pData)[0],static_cast<const float*>(mapped.pData)[1],
                static_cast<const float*>(mapped.pData)[2],static_cast<const float*>(mapped.pData)[3]);
        else
        {
            const unsigned char* p = static_cast<const unsigned char*>(mapped.pData);
            Log("overlay probe @%u,%u = %u %u %u %u  (fmt=%u target)",x,y,p[0],p[1],p[2],p[3],unsigned(bd.Format));
        }
        context->Unmap(staging.Get(),0);
    }

    bool EnsureTexture(UINT w,UINT h)    {
        if (texture && width == w && height == h) return true;
        texture.Reset(); textureSrv.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        // GDI hands us BGRA bytes, so ask for a BGRA texture and let the view
        // swizzle for free instead of swapping red and blue.
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(deviceRef->CreateTexture2D(&td,nullptr,&texture))) return false;
        if (FAILED(deviceRef->CreateShaderResourceView(texture.Get(),nullptr,&textureSrv))) return false;
        width = w; height = h;
        return true;
    }

    bool Draw(const Settings& settings,const Stats& stats)
    {
        const auto& rows = Rows();
        if (screenWidth < 600 || screenHeight < 400)
        {
            Log("overlay: window too small to draw (%ux%u)",screenWidth,screenHeight);
            return false;
        }

        const int panelW = 560;
        const int panelH = int(headerHeight + padding * 2 + rows.size() * rowHeight + footerHeight);
        panelX = (int(screenWidth) - panelW) / 2;
        panelY = (int(screenHeight) - panelH) / 2;
        if (panelX < 0) panelX = 0;
        if (panelY < 0) panelY = 0;
        if (!EnsureTexture(UINT(panelW),UINT(panelH))) return false;

        HDC dc = CreateCompatibleDC(nullptr);
        if (!dc) { Log("overlay: CreateCompatibleDC failed"); return false; }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = panelW;
        info.bmiHeader.biHeight = -panelH;   // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
        if (!bitmap || !bits) { Log("overlay: CreateDIBSection failed"); DeleteDC(dc); return false; }
        HGDIOBJ oldBitmap = SelectObject(dc,bitmap);

        // Background + border.
        RECT full{0,0,panelW,panelH};
        HBRUSH background = CreateSolidBrush(RGB(18,22,30));
        FillRect(dc,&full,background);
        DeleteObject(background);
        HBRUSH border = CreateSolidBrush(RGB(70,140,200));
        RECT top{0,0,panelW,3}; FillRect(dc,&top,border);
        DeleteObject(border);

        SetBkMode(dc,TRANSPARENT);

        HFONT titleFont = CreateFontW(-22,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
                                      CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
        HFONT rowFont = CreateFontW(-17,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
                                    CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
        HFONT smallFont = CreateFontW(-14,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
                                      CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");

        HGDIOBJ previous = SelectObject(dc,titleFont);
        SetTextColor(dc,RGB(235,245,255));
        wchar_t title[160]{};
        swprintf_s(title,L"Diva Native DLSS  ·  %ls   [F7 关闭]",stats.backendName?stats.backendName:L"-");
        TextOutW(dc,16,10,title,int(wcslen(title)));

        SelectObject(dc,rowFont);
        int y = int(headerHeight) + padding;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const bool selected = int(i) == selection;
            RECT line{6,y,panelW - 6,y + rowHeight - 4};
            if (selected)
            {
                HBRUSH highlight = CreateSolidBrush(RGB(34,86,140));
                FillRect(dc,&line,highlight);
                DeleteObject(highlight);
            }
            SetTextColor(dc,selected ? RGB(255,255,255) : RGB(196,206,218));
            TextOutW(dc,18,y + 6,rows[i].label,int(wcslen(rows[i].label)));

            const std::wstring value = ValueText(settings,stats,rows[i].id);
            SIZE extent{};
            GetTextExtentPoint32W(dc,value.c_str(),int(value.size()),&extent);
            SetTextColor(dc,rows[i].kind == Kind::Info ? RGB(150,200,235) : RGB(240,250,255));
            TextOutW(dc,panelW - 22 - extent.cx,y + 6,value.c_str(),int(value.size()));

            if (rows[i].kind == Kind::Integer || rows[i].kind == Kind::Enum)
            {
                SetTextColor(dc,RGB(110,125,145));
                TextOutW(dc,panelW - 30,y + 6,L"›",1);
            }
            y += rowHeight;
        }

        SelectObject(dc,smallFont);
        SetTextColor(dc,RGB(130,142,158));
        wchar_t footer[220]{};
        swprintf_s(footer,L"↑↓ 选择   ←→ 调整   Enter 执行   鼠标可点击   配置文件 settings.ini%ls%ls",
                   (stats.detail && *stats.detail) ? L"   ·   " : L"",
                   (stats.detail && *stats.detail) ? stats.detail : L"");
        TextOutW(dc,16,panelH - int(footerHeight) + 6,footer,int(wcslen(footer)));

        SelectObject(dc,previous);
        DeleteObject(titleFont);
        DeleteObject(rowFont);
        DeleteObject(smallFont);
        GdiFlush();

        // GDI does not manage the alpha channel of a 32bpp DIB, so a panel it
        // drew can end up fully transparent under SRC_ALPHA blending. The panel
        // is opaque by design, so force the alpha byte.
        {
            unsigned char* bytes = static_cast<unsigned char*>(bits);
            for (int i = 3; i < panelW * panelH * 4; i += 4) bytes[i] = 0xFF;
        }

        if (!uploadContext) deviceRef->GetImmediateContext(&uploadContext);
        if (uploadContext) uploadContext->UpdateSubresource(texture.Get(),0,nullptr,bits,panelW * 4,0);

        SelectObject(dc,oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
        return true;
    }

public:
    struct Request { bool resetHistory = false; bool reloadSettings = false; };
    // Consumed by the renderer after Render().
    Request request;
    bool ConsumeClick() { const bool value = pendingClick; pendingClick = false; return value; }

private:
    ComPtr<ID3D11Device> deviceRef;
    ComPtr<ID3D11DeviceContext> uploadContext;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> textureSrv;
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11BlendState> blend;

    static const int headerHeight = 40;
    static const int padding = 8;
    static const int rowHeight = 26;
    static const int footerHeight = 30;

    bool initialized = false;
    bool visible = false;
    bool rebuild = true;
    bool pendingClick = false;
    int selection = 0;
    int probeLeft = 0;
    int mouseX = 0,mouseY = 0;
    int panelX = 0,panelY = 0;
    UINT width = 0,height = 0;
    unsigned int screenWidth = 0,screenHeight = 0;
    UINT64 lastStatsSample = 0,flashUntil = 0;
};
