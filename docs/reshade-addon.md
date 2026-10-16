# Moving the configuration panel into ReShade (plan + verified facts)

## Status: implemented

`src_reshade/addon.cpp` + `src_reshade/reshade_min.hpp` build to
`DivaNativeDLSS.addon64`, which is installed to the game root next to
`Luma-Project Diva Mega Mix.addon`. Build export set matches Luma's exactly:

```
AddonInit  AddonUninit  NAME  DESCRIPTION  WEBSITE
```

Two version traps were solved by not including ReShade's own headers at all:

* ReShade master's `RESHADE_API_VERSION` is **20**, but the installed ReShade
  6.6.2 only accepts **18** (`Registered add-on "Luma" ... using ReShade API
  version 18`). `reshade::register_addon()` would therefore have been rejected;
  the add-on calls `ReShadeRegisterAddon(module, 18)` directly.
* Including `imgui.h` trips `#if IMGUI_VERSION_NUM != 19250 #error` in master's
  `reshade_overlay.hpp`, while 6.6.2 hands out a **19222** table. The add-on
  skips that header and asks `ReShadeGetImGuiFunctionTable(19222)` itself, then
  indexes the table as a flat `void* fn[421]` array - layout-identical, with the
  indices extracted programmatically from the v6.6.2 header.

---

The mod's own in-game panel could not be made to appear on the game's back
buffer, while the status toast drawn through the same path does appear. Rather
than keep chasing that, the config UI moves into ReShade's overlay, which is
already proven to render in this game (the Luma add-on uses it).

This file records everything verified about the ReShade side so the work can be
resumed without repeating the reconnaissance.

## Verified: how ReShade loads add-ons here

ReShade 6.6.2 (unofficial), add-on API version 18.

```
ReShade.log:
  Searching for add-ons (*.addon, *.addon64) in '<game root>' ...
  Loading add-on from '<game root>\Luma-Project Diva Mega Mix.addon' ...
  Registered add-on "Luma" v0.0.0.0 using ReShade API version 18.
```

* The DLL lives in the **game root**, named `*.addon` or `*.addon64`.
* Required exports, confirmed with `dumpbin /exports` on the Luma add-on:

  ```
  AddonInit        bool AddonInit(HMODULE addon_module, HMODULE reshade_module)
  AddonUninit      void AddonUninit(HMODULE addon_module, HMODULE reshade_module)
  NAME / DESCRIPTION / WEBSITE    (string exports)
  ```

## Verified: the exact entry point signatures

Taken from ReShade's own source, not guessed. A wrong signature here crashes
the game during add-on load:

```cpp
bool ReShadeRegisterAddon(void *module, uint32_t api_version);
void ReShadeUnregisterAddon(void *module);
void ReShadeRegisterOverlay(const char *title, void(*callback)(reshade::api::effect_runtime *runtime));
const void *ReShadeGetImGuiFunctionTable(uint32_t version);
void ReShadeLogMessage(void *module, int level, const char *message);   // three arguments!
```

`ReShadeLogMessage` taking the add-on module as its **first** argument was
written as `(int, const char *)` on the first attempt. That made ReShade read the
string pointer as the log level and a garbage stack value as the message, and the
game died with the log stopping right after `Loading add-on from ...`.
`source/addon.cpp:15` has the real signature.

## Verified: NAME/DESCRIPTION/WEBSITE are pointers, not arrays

This one crashes on its own, so fixing the log signature did not help.
`source/addon_manager.cpp:483`:

```cpp
if (const char *const *name = reinterpret_cast<const char *const *>(GetProcAddress(module, "NAME")))
    info.name = *name;   // dereferences once
```

ReShade treats the exported symbol as a `const char *const *` and dereferences it,
so the symbol address must hold a **string pointer**:

```cpp
extern "C" __declspec(dllexport) const char *NAME = "Diva Native DLSS";   // correct
// extern "C" __declspec(dllexport) const char NAME[] = "...";            // crashes
```

Exporting the character array instead hands ReShade the first eight characters of
the string reinterpreted as a pointer; it then builds a `std::string` from that
address and dies. The same applies to `AUTHOR`, `DESCRIPTION`, `WEBSITE` and
`ISSUES`.

## Version negotiation on ReShade 6.8

`ReShadeGetImGuiFunctionTable` serves several table generations at once
(`source/imgui_function_table.cpp`):

```cpp
if (version == 19250) return &g_imgui_function_table_19250;
if (version == 19220 || version == 19222) return &g_imgui_function_table_19222;
```

So asking for 19222 on ReShade 6.8.0 is valid and returns the genuine 19222
layout. `ReShadeRegisterAddon` accepts any API version whose major matches
(`api_version / 10000 == RESHADE_API_VERSION / 10000`), so 18 is accepted by a
build whose `RESHADE_API_VERSION` is 20.



`dumpbin /exports dxgi.dll` (477 exports, these are the relevant ones):

```
ReShadeRegisterAddon              ReShadeRegisterOverlay
ReShadeRegisterEvent              ReShadeRegisterOverlayForAddon
ReShadeRegisterEventForAddon      ReShadeGetImGuiFunctionTable
ReShadeGetBasePath                ReShadeGetConfigValue
ReShadeLogMessage                 ReShadeSetConfigArray
ReShadeCreateEffectRuntime        ReShadeDestroyEffectRuntime
```

`ReShadeGetImGuiFunctionTable` is the only ImGui entry point - add-ons never
link ImGui directly.

## Verified: the ImGui version is locked

`include/reshade_overlay.hpp` (a.k.a. `addon_imgui.hpp`) opens with:

```cpp
#if IMGUI_VERSION_NUM != 19222
#error Unexpected ImGui version, please update the "imgui.h" header to version 19222!
#endif
static_assert(sizeof(ImTextureID) == 8, ...);
```

So the build needs **ImGui 1.92.2** headers (`imgui.h`, plus `imgui_internal.h`
for the internals the header touches: `ImGuiStorage`, `ImDrawList`, `ImFont`,
`ImGuiListGenerator`-family, `ImDrawListSharedData`, `ImFontBaked`).

The UI itself is a function table, `imgui_function_table_19222`, obtained from
`ReShadeGetImGuiFunctionTable`. Buttons, checkboxes, combos and sliders are
function pointers in that table - the add-on calls those, not `ImGui::*`.

## Blocking issue encountered

`reshade_api.hpp` and `reshade_overlay.hpp` are large and the tooling used here
truncates very large HTTP responses mid-file (`reshade_overlay.hpp` came back
with a chunk omitted; `reshade_api.hpp` came back incomplete). Vendoring the four
headers reliably needs a session with unrestricted file downloads, e.g.:

```powershell
git clone --depth 1 --branch v6.6.2 https://github.com/crosire/reshade tmp
# copy include/*.hpp and deps/imgui/{imgui.h,imgui_internal.h}
```

or a release-zip download. Until that is done, writing the add-on would mean
guessing an ABI, which is not worth the risk.

## Design: no IPC needed, settings.ini is the interface

```
ReShade overlay  (game root / DivaNativeDLSS.addon64)
      |  read/write
      v
mods\Diva Native DLSS\settings.ini
      |  hot reload (already implemented: every 120 frames, plus on demand)
      v
DivaNativeDLSS.dll  (DML mod - rendering, upscaling, unchanged)
```

Both sides are independent. If the add-on misbehaves, rendering, F4/F5/F6 and the
toast are unaffected.

Panel contents map one-to-one onto the existing rows:

| Setting | Widget |
| --- | --- |
| `[General] Backend` | combo: Auto / FSR 4.1 / DLSS / Off |
| `[FSR] Sharpness` | slider 0-100 |
| `[FSR] ColorSpace` | combo: Auto / sRGB / PQ (HDR10) / linear scRGB |
| `[FSR] MotionVectorScale` | slider 25-400 |
| `[FSR] MotionVectorSignY` | checkbox |
| `[FSR] DebugView` | checkbox |
| `[General] DebugMotion` | checkbox |
| `[General] ShowStatus` | checkbox |
| `[General] HistoryMB` | slider 32-512, step 32 |
| `[General] TraceOnly` / `CaptureOnly` | checkboxes |
| reset history | button |
| reload / save ini | buttons |
| live status | read-only text (resolution + SDR/HDR, backend, published frames) |

## Suggested order of work

1. Vendor the headers (see above). Build a **bare add-on that only registers an
   empty overlay tab** and confirm it appears in ReShade's overlay in-game
   before writing any widget code. This is the "can it show at all" gate - the
   mistake made with the in-engine panel was writing a full implementation on
   top of an unverified assumption.
2. Add the widgets, reading `settings.ini` on open and writing it on change.
3. Add a CMake target producing `DivaNativeDLSS.addon64` plus an install rule
   that copies it to the game root (DML's mod folder stays as it is).
4. Keep `settings.ini` as the only coupling; do not add shared memory or IPC.

## Do not repeat

Two verification mistakes were made on the in-engine panel and are worth
avoiding here:

* A test that counted "pixels differing from the scene colour" passed for the
  wrong reason (it counted the black background), which made a panel that never
  rendered look like it worked. Assert on the panel's own colours instead.
* An "install" that copied the release package onto itself while the game was
  running reported matching hashes and changed nothing. Compare against the
  build output, not against the release package.
