# FSR 4.1 backend (AMD RDNA4 / RX 9070 XT)

This document covers the AMD path: how it works, what it needs, what was
measured on real hardware, and where it is still weak.

## Why a D3D11 -> D3D12 bridge is unavoidable

Project DIVA Mega Mix+ renders with **D3D11**, and this mod hooks that context.
AMD's FSR 4 upscaler does not have a D3D11 backend at all: the FidelityFX 2.x
API only ships a DX12 (and Vulkan/Xbox) provider, and `FfxApiResource.resource`
must be an `ID3D12Resource*`.

So the mod creates a second **D3D12 device on the game's own adapter** and
passes surfaces across the API boundary:

```
   D3D11 (the game)                          D3D12 (FSR 4.1)
   ----------------                          ---------------
   shared colour  (R8G8B8A8_UNORM)  --\
   shared depth   (R32_FLOAT)          |--> ffxDispatchDescUpscale
   shared motion  (R16G16_FLOAT)    --/         |
   shared output  (R8G8B8A8_UNORM + UAV) <------+
```

Surfaces are created on the D3D11 side with `D3D11_RESOURCE_MISC_SHARED`,
handed over through `IDXGIResource::GetSharedHandle` and re-opened with
`ID3D12Device::OpenSharedHandle`. A pair of **shared fences** orders the two
queues: D3D11 signals "inputs written", the D3D12 queue waits on it, runs the
upscaler, signals "output ready", and the D3D11 context waits before copying the
result back.

## Sharing flags: measured, not guessed

The obvious choice (`D3D11_RESOURCE_MISC_SHARED_NTHANDLE`) **does not work**
here. Measured on an RX 9070 XT with driver 32.0.32015.2008:

| Recipe | Texture creation | D3D11 write reaches D3D12 | Keeps `ALLOW_UNORDERED_ACCESS` |
| --- | --- | --- | --- |
| `SHARED_NTHANDLE` alone | rejected, `E_INVALIDARG` | – | – |
| `SHARED_NTHANDLE \| SHARED_KEYEDMUTEX` | ok | **silently lost** unless `AcquireSync` is used, and D3D12 cannot expose `IDXGIKeyedMutex` at all | yes |
| `RESOURCE_MISC_SHARED` (legacy) | ok | **yes**, with or without a barrier | yes |

Two traps worth knowing:

* `ID3D11DeviceContext::UpdateSubresource` into a keyed-mutex shared texture
  **silently discards the data**; `CopyResource` works. The mod only ever uses
  `CopyResource` for these surfaces.
* `ID3D12Device::OpenSharedFence` does not exist - shared fences go through
  `OpenSharedHandle` like everything else.

`tools/bridge_final.cpp` and `tools/interop_probe.cpp` reproduce all of this.

## Getting the upscaler runtime

The mod does **not** ship `amd_fidelityfx_upscaler_dx12.dll`; it is looked up at
runtime, in this order:

1. `FSR\UpscalerDll` in `settings.ini`, or the `DIVA_FSR_DLL` environment variable
2. the mod folder, the game folder, `<game>\D3D12_Optiscaler\`, `System32`
3. the Steam libraries on the machine - and when several games ship a copy, the
   **newest build wins**, so an FSR 4.1 copy is preferred over an older 4.0 one

Most players already have one: FSR 4 titles such as Battlefield 6, Black Myth:
Wukong, Cyberpunk 2077 and others install it next to their executable.

If none is present, supply the file yourself. This project does not download or
redistribute any runtime - copy `amd_fidelityfx_upscaler_dx12.dll` into the mod
folder, the game root, or `D3D12_Optiscaler/`. The legitimate source is the
[FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)
release archive, where it is `Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll`,
or the install directory of any FSR 4 title you own.

Per AMD's `docs/license.md`, the upscaler DLL and the FidelityFX headers are
covered by the MIT exception list in that file, so redistributing them with the
mod is permitted as long as AMD's notice is kept. See `THIRD-PARTY-NOTICES.md`.

## Depth and motion vectors

FSR requires depth as **single-channel FLOAT**; `D24S8` and `D16` are not
accepted. The game's depth buffer is typeless and may be any of `D32`, `D24` or
`D16`, so the backend never copies it raw: a one-triangle pass samples the
readable depth view and writes `R32_FLOAT` into the shared surface. That removes
the whole typeless/typed compatibility question.

Motion vectors come from the existing geometry-reconstruction path and are
already in pixels with the frame's jitter subtracted, so:

* `FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION` is **not** set (the
  shader already cancels jitter),
* `FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS` **is** set,
* `FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE` is set by default because the game
  backbuffer is gamma encoded.

Only **Native AA** (1.0x, `FFX_UPSCALE_QUALITY_MODE_NATIVEAA`) is used. This mod
cannot lower the game's internal render resolution, so the ratio modes
(Quality/Balanced/Performance/Ultra Performance) are not reachable here.

## Settings

`mods/Diva Native DLSS/settings.ini`

```ini
[General]
Backend=0            ; 0=Auto 1=FSR 4.1 2=DLSS/DLAA 3=Off
ShowStatus=1
DebugMotion=0
TraceOnly=0
CaptureOnly=0
HistoryMB=128
OverlayKey=118       ; 0x76 = F7

[FSR]
Sharpness=0              ; 0..100, feeds FSR's RCAS pass
NonLinearColorSpace=1    ; backbuffer is gamma encoded
MotionVectorScale=100    ; percent; main tuning knob
MotionVectorSignY=1      ; +1 / -1, motion vector Y convention
DebugView=0              ; FSR's own debug visualisation
UpscalerDll=             ; optional explicit path to the upscaler DLL
```

`Auto` picks FSR 4.1 when an upscaler DLL is found, otherwise DLSS if this build
has the NGX backend, otherwise it does nothing.

## HDR

Project DIVA Mega Mix+ presents in **HDR10** (`R10G10B10A2_UNORM`, `format=10` in
the log) when the machine is in HDR. The mod detects that from the format the
game renders into and switches the whole working chain:

| Backbuffer | Working chain | Transfer function | FFX create flags |
| --- | --- | --- | --- |
| `R8G8B8A8_UNORM` | `R8G8B8A8_UNORM` | sRGB (non-linear) | `NON_LINEAR_COLORSPACE` |
| `R10G10B10A2_UNORM` | `R16G16B16A16_FLOAT` | PQ / HDR10 | `+ HIGH_DYNAMIC_RANGE` |
| `R16G16B16A16_FLOAT` | `R16G16B16A16_FLOAT` | linear scRGB | `HIGH_DYNAMIC_RANGE` only |

The dispatch also declares the transfer function
(`FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_PQ` for HDR10,
`_SRGB` for SDR, nothing for scRGB). `ColorSpace` in `settings.ini`, or the
"色彩空间 Color space" row in the F7 panel, overrides the automatic choice for
setups whose container format does not match its transfer function
(`0` auto, `1` sRGB, `2` PQ, `3` linear scRGB).

Verified with `tools/fsr_test.exe <dll> hdr`, which drives the real backend with
a 16-bit float chain and reports whether the GPU survived:

```
FSR bridge: shared surfaces 1280x720 fmt=10 created   <- 10 = R16G16B16A16_FLOAT
FSR context created; version 16781313 flags=259       <- 0x103 = HDR | native MV | non-linear
Run #1 ok (lastResult=0x00000000 frames=1)
device removed reason after HDR dispatch = 0x00000000
```

## Configuration panel (F7)

`F7` opens an in-game panel: choose the backend, tune sharpness, colour space and
motion vector scaling, flip diagnostics, reset the history, reload/save the ini,
and watch live FPS, extent (with SDR/HDR) and backend status. Arrow keys
navigate, `Enter`/`Right` changes a value, the mouse also works, `F7`/`Esc`
closes it. Changes are written to `settings.ini` immediately.

The panel is rendered with GDI into a DIB and uploaded as one texture, so it
uses the same font stack as the status toast and costs a single textured
triangle per frame. GDI does not write the alpha channel of a 32bpp DIB, so the
alpha byte is forced opaque after drawing - otherwise the panel blends away to
nothing. The panel's own pipeline state is saved and restored around it, since
it rebinds shaders, viewports and blend/depth/raster state.

The mod logs the first 40 key presses (`key down vk=...`) so a hotkey that never
arrives at the window procedure is immediately visible in the log.

Note: while the panel is open the mod swallows the navigation keys it uses, but
that only covers window-message input. A game reading raw input directly may
still see them.

## Validation performed

On an RX 9070 XT (RDNA4, driver 32.0.32015.2008) against the real FSR 4.1
runtime (`amd_fidelityfx_upscaler_dx12.dll` 4.1.0.0):

* `tools/fsr_test.exe` - creates a 1280x720 frame, runs the real backend, and
  reads the result back:

  ```
  FSR context created; version 16781313 flags=258 extent=1280x720
  Run #1 ok (lastResult=0x00000000 frames=1)
  output non-black pixels 921600 / 921600
  output mean channel    123.6
  mean abs diff vs input 2.3 / 255     <- Native AA is near identity on a static frame
  ```

* `gpu_test.exe sr-uidepth-pause-keys-overlay .\DivaNativeDLSS.dll` - the full
  mod, FSR backend, 24 published frames, UI preservation, pause retention,
  resize 1280x720 -> 1600x900, F4 semantics, and the F7 panel opening,
  rendering (1040/1600 scanline pixels), persisting a change and closing.

## Limitations

* **FSR 4.1 requires RDNA4** (RX 9000). RDNA3 desktop is supported from SDK 2.3;
  RDNA2 is announced for 2027. On other GPUs the backend will fail to create a
  context and the mod leaves the frame untouched.
* The D3D11 -> D3D12 bridge costs one extra full-resolution copy in and one out
  per frame, plus a GPU-side wait. It is not free; measure before enabling on a
  slow GPU.
* `MotionVectorScale` / `MotionVectorSignY` are the knobs to reach for if the
  image smears. They are exposed in the panel precisely because the correct
  value depends on the game's motion vector convention, and that could not be
  settled without the retail game.
* Sharpening and non-linear colour space were validated for stability, not
  tuned for taste.
* Everything inherits the geometry-reconstruction limitations: skipped draws
  (geometry/hull/domain shaders, stream output, odd viewports, instancing past
  64) get zero motion vectors and therefore ghost.

## ReShade + the Luma add-on: why the device is built through the factory

With ReShade resident and the **Luma** add-on loaded, calling
`d3d12.dll!D3D12CreateDevice` kills the game:

> Luma - The application tried to create a non DirectX 11 device. Luma currently
> only supports DirectX 11, the application might crash.

ReShade hooks `d3d12.dll!D3D12CreateDevice` and redirects it
(`Redirecting D3D12CreateDevice(...)` in `ReShade.log`), which raises ReShade's
`init_device` for the new D3D12 device; Luma sees a non-D3D11 device and aborts.

The way around it is the Agility SDK device factory, which does not touch that
export at all:

```
d3d12.dll!D3D12GetInterface(CLSID_D3D12DeviceFactory, IID_ID3D12DeviceFactory)
  -> ID3D12DeviceFactory::CreateDevice(adapter, featureLevel, ...)
```

Two ordering details, both measured with `tools/dx12_factory_probe.cpp`:

* `d3d12core.dll!D3D12GetInterface` returns `E_NOINTERFACE` (0x80004002) on a
  cold call - it only works after the runtime has bootstrapped the factory, so
  `d3d12.dll` has to be asked first.
* `d3d12.dll` is the only module that can serve the *first* call, so the bridge
  tries `d3d12.dll` then `d3d12core.dll` and logs which one answered.

`D3D12CreateDevice` is still the last-resort fallback for pre-1903 Windows,
where no device factory exists; on such a system Luma and FSR4 cannot coexist.

