// ReShade add-on that provides the configuration panel for Diva Native DLSS.
//
// It is deliberately decoupled from the mod: both sides only ever touch
// `mods\Diva Native DLSS\settings.ini`, which the DLL already hot-reloads
// (every 120 frames and immediately after a change). No shared memory, no
// window messages, no IPC.
//
//   ReShade overlay  ->  settings.ini  ->  DivaNativeDLSS.dll (DML mod)
//
// ReShade loads add-ons from the game directory, so this builds to
// `DivaNativeDLSS.addon64` and does not interact with DivaModLoader at all.
#include "reshade_min.hpp"

#include <cstdio>
#include <string>

using namespace reshade_min;

namespace {

Api g_api;
HMODULE g_addonModule = nullptr;
std::wstring g_iniPath;
std::wstring g_modDirectory;

std::wstring ModuleDirectory(HMODULE module)
{
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return {};
    std::wstring s(path);
    const size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : s.substr(0, slash);
}

std::string Narrow(const std::wstring &text)
{
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(size_t(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

int ReadInt(const wchar_t *section, const wchar_t *key, int fallback)
{
    return int(GetPrivateProfileIntW(section, key, UINT(fallback), g_iniPath.c_str()));
}

void WriteInt(const wchar_t *section, const wchar_t *key, int value)
{
    wchar_t text[32]{};
    swprintf_s(text, L"%d", value);
    WritePrivateProfileStringW(section, key, text, g_iniPath.c_str());
}

// Everything the panel edits, read once per frame so external edits show up too.
struct Values
{
    int backend = 0, showStatus = 1, debugMotion = 0, historyMB = 128;
    int traceOnly = 0, captureOnly = 0;
    int sharpness = 0, colorSpace = 0, mvScale = 100, mvSignY = 1, debugView = 0;

    void Load()
    {
        backend     = ReadInt(L"General", L"Backend", 0);
        showStatus  = ReadInt(L"General", L"ShowStatus", 1);
        debugMotion = ReadInt(L"General", L"DebugMotion", 0);
        historyMB   = ReadInt(L"General", L"HistoryMB", 128);
        traceOnly   = ReadInt(L"General", L"TraceOnly", 0);
        captureOnly = ReadInt(L"General", L"CaptureOnly", 0);
        sharpness   = ReadInt(L"FSR", L"Sharpness", 0);
        colorSpace  = ReadInt(L"FSR", L"ColorSpace", 0);
        mvScale     = ReadInt(L"FSR", L"MotionVectorScale", 100);
        mvSignY     = ReadInt(L"FSR", L"MotionVectorSignY", 1);
        debugView   = ReadInt(L"FSR", L"DebugView", 0);
    }

    void Save() const
    {
        WriteInt(L"General", L"Backend", backend);
        WriteInt(L"General", L"ShowStatus", showStatus);
        WriteInt(L"General", L"DebugMotion", debugMotion);
        WriteInt(L"General", L"HistoryMB", historyMB);
        WriteInt(L"General", L"TraceOnly", traceOnly);
        WriteInt(L"General", L"CaptureOnly", captureOnly);
        WriteInt(L"FSR", L"Sharpness", sharpness);
        WriteInt(L"FSR", L"ColorSpace", colorSpace);
        WriteInt(L"FSR", L"MotionVectorScale", mvScale);
        WriteInt(L"FSR", L"MotionVectorSignY", mvSignY);
        WriteInt(L"FSR", L"DebugView", debugView);
        // The legacy key the pre-FSR builds looked at, kept in sync.
        WriteInt(L"DLSS", L"Enable", backend == 3 ? 0 : 1);
    }
};

const char kBackendItems[]     = "Auto\0FSR 4.1\0DLSS / DLAA\0Off\0";
const char kColorSpaceItems[] = "Auto\0sRGB (SDR)\0PQ / HDR10\0Linear scRGB\0";

Values g_shown;      // what was last read from disk
bool   g_edited = false;

void DrawSettings(void * /*runtime*/)
{
    if (!g_api.imgui) return;

    Values v;
    v.Load();
    g_shown = v;
    bool changed = g_edited;
    g_edited = false;

    if (changed)
    {
        // keep the local copy in sync when something outside changed the file
    }

    const std::string ini = Narrow(g_iniPath);
    g_api.TextDisabled(ini.c_str());
    g_api.TextDisabled("The mod hot-reloads this file, so changes apply within a few frames.");
    g_api.Separator("Upscaler");

    bool enabled = v.backend != 3;
    if (g_api.Checkbox("Enable upscaling", &enabled)) { v.backend = enabled ? 1 : 3; changed = true; }
    g_api.TextDisabled(enabled ? "AMD FidelityFX Super Resolution 4.1, Native AA."
                               : "Upscaling disabled: the game's own image is presented unchanged.");

    g_api.Separator("FSR 4.1");

    if (g_api.SliderInt("Sharpness", &v.sharpness, 0, 100, "%d")) changed = true;
    if (g_api.Combo("Colour space", &v.colorSpace, kColorSpaceItems)) changed = true;
    g_api.TextDisabled("Auto detects from the swap chain format: 16F is linear scRGB,");
    g_api.TextDisabled("R10G10B10A2 is PQ / HDR10, R8G8B8A8 is sRGB.");
    if (g_api.SliderInt("Motion vector scale", &v.mvScale, 25, 400, "%d")) changed = true;
    g_api.TextDisabled("The main tuning knob if the image smears.");

    bool sign = v.mvSignY > 0;
    if (g_api.Checkbox("Motion vector Y is positive", &sign)) { v.mvSignY = sign ? 1 : -1; changed = true; }
    bool dbg = v.debugView != 0;
    if (g_api.Checkbox("Upscaler debug view", &dbg)) { v.debugView = dbg ? 1 : 0; changed = true; }

    g_api.Separator("Geometry reconstruction");

    bool motion = v.debugMotion != 0;
    if (g_api.Checkbox("Motion vector preview (F5)", &motion)) { v.debugMotion = motion ? 1 : 0; changed = true; }
    if (g_api.SliderInt("History size (MB)", &v.historyMB, 32, 512, "%d")) changed = true;

    bool trace = v.traceOnly != 0;
    if (g_api.Checkbox("Trace only (diagnostic)", &trace)) { v.traceOnly = trace ? 1 : 0; changed = true; }
    bool capture = v.captureOnly != 0;
    if (g_api.Checkbox("Capture only (diagnostic)", &capture)) { v.captureOnly = capture ? 1 : 0; changed = true; }
    bool status = v.showStatus != 0;
    if (g_api.Checkbox("On-screen status toast", &status)) { v.showStatus = status ? 1 : 0; changed = true; }

    g_api.Separator("Actions");

    if (g_api.Button("Save to settings.ini")) { v.Save(); changed = false; }
    if (g_api.Button("Reload from settings.ini")) { g_edited = true; changed = false; }

    if (changed) v.Save();
}

} // namespace

// These must be exported as POINTERS, not arrays. ReShade reads them with
//   const char *const *name = (const char *const *)GetProcAddress(module, "NAME");
//   info.name = *name;
// (addon_manager.cpp:483), so the symbol address has to hold a string pointer.
// Exporting `const char NAME[]` instead hands ReShade the first eight
// characters of the string reinterpreted as a pointer, and it dies building a
// std::string from that. Exactly Luma's layout.
extern "C" __declspec(dllexport) const char *NAME        = "Diva Native DLSS";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Switchable DLSS/DLAA and AMD FSR 4.1 configuration for Hatsune Miku: Project DIVA Mega Mix+.";
extern "C" __declspec(dllexport) const char *WEBSITE     = "https://github.com/xureimu-bit/diva-native-dlss";

extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE reshade_module)
{
    g_addonModule = addon_module;
    g_api.addonModule = addon_module;

    const std::wstring base = ModuleDirectory(addon_module);
    g_modDirectory = base + L"\\mods\\Diva Native DLSS";
    g_iniPath      = g_modDirectory + L"\\settings.ini";

    if (!g_api.Resolve(reshade_module))
    {
        return false;
    }
    // 18, not the 20 that ReShade master's headers would pass: the installed
    // ReShade 6.6.2 rejects anything newer.
    if (!g_api.registerAddon(addon_module, kApiVersion))
    {
        return false;
    }

    const void *table = g_api.getImGuiTable(kImGuiVersion);
    if (!table)
    {
        g_api.Message(LogError, "Diva Native DLSS: this ReShade build has no ImGui 1.92.2 table; panel disabled.");
        g_api.unregisterAddon(addon_module);
        return false;
    }
    g_api.imgui = static_cast<const ImGuiTable *>(table);

    g_api.registerOverlay("Diva Native DLSS", &DrawSettings);
    g_api.Message(LogInfo, "Diva Native DLSS configuration panel registered.");
    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE /*reshade_module*/)
{
    if (g_api.unregisterAddon) g_api.unregisterAddon(addon_module);
    g_api.imgui = nullptr;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
