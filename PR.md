# ReShade configuration panel, 1080p fix, and driver-reset fixes

Three problems, each reproduced and then verified fixed on an RX 9070 XT (RDNA4, driver
32.0.32015.2008) with a real FSR 4.1 runtime.

## 1. The mod did nothing at 1920×1080

At 1080p the game reserves a 1920×1088 viewport and a 1920×1088 depth buffer while the swap
chain is 1920×1080 — 1080 is not a multiple of 16, so the game rounds up. Three size checks
in the mod compared against `screenWidth`/`screenHeight` for exact equality, so every draw
was rejected and the mod was silently inert: no jitter, no geometry capture, nothing
published. At 4K the game uses 2160 lines, which is already a multiple of 16, so the
mismatch never appeared and the bug stayed hidden.

Instrumenting every early return on the capture path was what finally located it — the
rejections that did show up were all bloom's mip chain, and the main pass never reached the
classification at all.

The checks now accept a viewport, depth buffer or render target that *covers* the screen
within a small tolerance (`CoversScreen`), which also covers any other resolution whose
viewport height differs from the swap chain.

Verified: `jittered`, `geometry` and `publish` all go from 0 to non-zero at 1080p.

## 2. Changing a setting could reset the display driver

Turning the upscaler off, changing the colour space, or toggling the debug view all destroy
and recreate the FSR context and its shared surfaces. Nothing waited for the D3D12 queue
first, so surfaces the GPU was still reading got released — device removed.

Every step inside a frame already waits on the bridge fences (`WaitForInputsFree`,
`WaitForOutput`); teardown did not. Added `Dx12Bridge::IdleWait()`, which signals the
D3D12->D3D11 fence on the queue and blocks until it is reached, and called it before
`DestroyContext()` on all three rebuild paths (`Invalidate`, `Shutdown`, and `Ready`'s
recreation path — the last one is what the colour space and debug view toggles go through).

Verified: `FSR bridge: idle wait complete before teardown` precedes every context rebuild,
and repeated colour space / debug view / on-off switching no longer removes the device.

## 3. The configuration panel had no way to appear

The in-engine panel draws into `swap->GetBuffer(0)` and, for reasons never established,
those writes never reached the presented surface while the status toast drawn through the
same path did. Rather than keep chasing it, the configuration UI is now a **ReShade add-on**
(`DivaNativeDLSS.addon64`, placed in the game root) using ReShade's own ImGui overlay, which
the Luma add-on already proves renders in this game.

The two sides are coupled only through `settings.ini`, which the mod hot-reloads, so no IPC
is involved and a failing add-on cannot affect rendering. ReShade stays optional.

The add-on deliberately includes no ReShade or ImGui headers. All ABI is declared by hand in
`src_reshade/reshade_min.hpp` because master's `RESHADE_API_VERSION` is 20 while 6.6.2 only
accepts 18, and including `imgui.h` trips a version lock that 6.6.2's 19222 table fails.
`docs/reshade-addon.md` records the exact signatures, the export layout (`NAME` must be a
pointer, not an array) and the version negotiation, all taken from ReShade's source rather
than guessed.

Also verified against ReShade 6.8.0.

## Smaller changes

- Announce the backend once on the first frame. A config left on `Off` was indistinguishable
  from a broken mod: nothing on screen, no error, and the log only revealed it if you knew
  which counter to read.
- Reset `overlayDepth` at the frame boundary. `pass_sprite` is a per-frame annotation, and a
  missed `EndEvent` left every later draw classified as UI.
- Declare the colour space flags explicitly: `R16G16B16A16_FLOAT` is linear scRGB,
  `R10G10B10A2` is PQ/HDR10, `R8G8B8A8` is sRGB. Feeding linear scRGB while declaring PQ
  crushes every highlight.

## What is not included

- **Upscaling quality tiers.** A downscale-then-upscale path was implemented and did work
  (4K: 2560×1440 input into a 3840×2160 output, bridge surfaces split accordingly), but it
  cropped the image into a corner and could reset the driver when switched, so it is
  disabled. Note it only removes the *upscaler's* cost — the game still rasterises at full
  resolution.
- **Skipping the game's own MLAA/FXAA.** The game only lets you choose between the two, so
  everything the upscaler sees is already spatially filtered. The AA pass was identified (it
  is the third from the end of the per-frame post-processing chain, and its pixel shader is
  the one that changes when you switch the option), but the replacement is not implemented.
- **NGX/DLSS remains unverified here** — no NVIDIA SDK was available, so the build falls back
  to FSR only. The backend code is untouched.
