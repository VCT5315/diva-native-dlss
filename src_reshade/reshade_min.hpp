#pragma once
// Minimal ReShade add-on ABI, declared by hand on purpose.
//
// Two version traps are avoided this way:
//
//   * ReShade master's reshade.hpp uses RESHADE_API_VERSION 20, but the
//     installed ReShade 6.6.2 only accepts 18 (it logs "using ReShade API
//     version 18"), so reshade::register_addon() would be rejected.
//   * Including imgui.h would trip `#if IMGUI_VERSION_NUM != 19250 #error`
//     in master's reshade_overlay.hpp, while 6.6.2 hands out a 19222 table.
//
// ReShade's ImGui function table is a flat array of function pointers, so an
// index into `void* fn[N]` is layout-identical to the real struct. The indices
// below were extracted programmatically from the v6.6.2 header.
#include <windows.h>
#include <cstdint>

namespace reshade_min {

constexpr uint32_t kApiVersion      = 18;    // what ReShade 6.6.2 accepts
constexpr uint32_t kImGuiVersion    = 19222; // what ReShade 6.6.2 hands out
constexpr int      kImGuiFieldCount = 421;   // v6.6.2 table length

struct ImVec2f { float x = 0.0f, y = 0.0f; };

struct ImGuiTable { void* fn[kImGuiFieldCount]; };

// Indices into imgui_function_table_19222.
enum : int {
    iTextUnformatted = 103,
    iTextDisabledV   = 106,
    iSeparatorText   = 110,
    iButton          = 111,
    iCheckbox        = 115,
    iCombo2          = 130,
    iSliderInt       = 149,
    iBeginDisabled   = 279,
    iEndDisabled     = 280,
};

using PfnRegisterAddon   = bool (*)(void *, uint32_t);
using PfnUnregisterAddon = void (*)(void *);
using PfnGetImGuiTable   = const void *(*)(uint32_t);
using PfnRegisterOverlay = void (*)(const char *, void (*)(void *));
// Signature verified against ReShade's source (source/addon.cpp):
//   void ReShadeLogMessage(void *module, int level, const char *message)
// It takes the add-on module first - calling it with two arguments passes a
// string pointer as the level and reads a garbage pointer as the message.
using PfnLogMessage      = void (*)(void *, int, const char *);

enum LogLevel { LogInfo = 1, LogWarning = 2, LogError = 3 };

struct Api
{
    PfnRegisterAddon   registerAddon   = nullptr;
    PfnUnregisterAddon unregisterAddon = nullptr;
    PfnGetImGuiTable   getImGuiTable   = nullptr;
    PfnRegisterOverlay registerOverlay = nullptr;
    PfnLogMessage      log             = nullptr;
    const ImGuiTable  *imgui           = nullptr;
    void              *addonModule     = nullptr;   // ReShadeLogMessage wants it

    bool Resolve(HMODULE reshade)
    {
        if (!reshade) return false;
        registerAddon   = reinterpret_cast<PfnRegisterAddon>  (GetProcAddress(reshade, "ReShadeRegisterAddon"));
        unregisterAddon = reinterpret_cast<PfnUnregisterAddon>(GetProcAddress(reshade, "ReShadeUnregisterAddon"));
        getImGuiTable   = reinterpret_cast<PfnGetImGuiTable>  (GetProcAddress(reshade, "ReShadeGetImGuiFunctionTable"));
        registerOverlay = reinterpret_cast<PfnRegisterOverlay>(GetProcAddress(reshade, "ReShadeRegisterOverlay"));
        log             = reinterpret_cast<PfnLogMessage>     (GetProcAddress(reshade, "ReShadeLogMessage"));
        return registerAddon != nullptr && registerOverlay != nullptr && getImGuiTable != nullptr;
    }

    void Message(LogLevel level, const char *text) const { if (log) log(addonModule, level, text); }

    // --- the handful of widgets the panel needs ---------------------------
    void Text(const char *text) const
    { reinterpret_cast<void (*)(const char *, const char *)>(imgui->fn[iTextUnformatted])(text, nullptr); }

    void TextDisabled(const char *text) const
    { reinterpret_cast<void (*)(const char *, const char *)>(imgui->fn[iTextUnformatted])(text, nullptr); }

    void Separator(const char *label) const
    { reinterpret_cast<void (*)(const char *)>(imgui->fn[iSeparatorText])(label); }

    bool Button(const char *label) const
    { const ImVec2f size{}; return reinterpret_cast<bool (*)(const char *, const ImVec2f &)>(imgui->fn[iButton])(label, size); }

    bool Checkbox(const char *label, bool *value) const
    { return reinterpret_cast<bool (*)(const char *, bool *)>(imgui->fn[iCheckbox])(label, value); }

    bool SliderInt(const char *label, int *value, int low, int high, const char *format) const
    { return reinterpret_cast<bool (*)(const char *, int *, int, int, const char *, int)>(imgui->fn[iSliderInt])(label, value, low, high, format, 0); }

    // items is a zero-separated, double-zero-terminated list.
    bool Combo(const char *label, int *current, const char *items) const
    { return reinterpret_cast<bool (*)(const char *, int *, const char *, int)>(imgui->fn[iCombo2])(label, current, items, -1); }

    void BeginDisabled(bool disabled) const
    { reinterpret_cast<void (*)(bool)>(imgui->fn[iBeginDisabled])(disabled); }

    void EndDisabled() const
    { reinterpret_cast<void (*)(void)>(imgui->fn[iEndDisabled])(); }
};

} // namespace reshade_min
