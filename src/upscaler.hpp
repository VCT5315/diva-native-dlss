#pragma once
#include "settings.hpp"

// Everything an upscaler needs for one frame. The geometry-reconstruction
// pipeline produces exactly these buffers no matter which vendor is behind it,
// which is what makes the two backends interchangeable at runtime.
struct UpscaleFrame
{
    ID3D11Device*        device   = nullptr;
    ID3D11DeviceContext* context  = nullptr;
    ID3D11Texture2D*     color    = nullptr; // game frame copy, R8G8B8A8_UNORM
    ID3D11Texture2D*     output   = nullptr; // upscaler result, R8G8B8A8_UNORM + UAV
    ID3D11Texture2D*     depth    = nullptr; // game depth copy (may be typeless)
    ID3D11ShaderResourceView* depthSrv = nullptr; // readable view of that depth
    DXGI_FORMAT          depthFormat = DXGI_FORMAT_UNKNOWN;
    ID3D11Texture2D*     motion   = nullptr; // R16G16_FLOAT, pixels, jitter removed
    UINT  width  = 0;
    UINT  height = 0;
    // Resolution the upscaler itself should run at. Equal to width/height for
    // Native AA; smaller for the ratio modes, in which case the caller supplies
    // colour and motion already downsampled to this size. The output stays at
    // width/height either way.
    UINT  renderWidth  = 0;
    UINT  renderHeight = 0;
    bool  reset  = false;
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    float frameTimeMs = 16.6667f;
    // True when the game presents a high dynamic range swap chain, in which
    // case the whole working chain is 16-bit float and the upscaler is told so.
    bool  hdr = false;
    // 0 auto | 1 sRGB (SDR) | 2 PQ (HDR10) | 3 linear (scRGB)
    int   colorSpace = 0;
};

class IUpscaler
{
public:
    virtual ~IUpscaler() {}
    // Short name shown in the configuration panel.
    virtual const wchar_t* DisplayName() const = 0;
    // One line describing the current state, for the panel and the log.
    virtual const wchar_t* StatusText() const = 0;
    // True once the backend proved it can run on this machine and resolution.
    virtual bool Available() const = 0;
    // True when a frame can be dispatched right now.
    virtual bool Ready(const UpscaleFrame& frame) = 0;
    // Dispatches the upscale. False means "leave the frame alone this time".
    virtual bool Run(const UpscaleFrame& frame) = 0;
    // Drops cached state without tearing the backend down (resize / setting change).
    virtual void Invalidate() = 0;
    virtual void Shutdown() = 0;

    unsigned int lastResult = 0;          // backend-specific status code
    unsigned long long frames = 0;        // successfully dispatched frames
};
