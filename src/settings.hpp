#pragma once
#include "common.hpp"

// Which upscaler turns the game's native-resolution frame into the final image.
// Both supported backends consume the same inputs (colour, depth, motion
// vectors, jitter), so switching between them is a runtime swap of one object.
enum class Backend : int
{
    Auto = 0, // pick FSR4 when available, otherwise DLSS, otherwise Off
    Fsr4 = 1,
    Dlss = 2,
    Off  = 3,
};

const wchar_t* BackendName(Backend b)
{
    switch (b)
    {
    case Backend::Auto: return L"Auto";
    case Backend::Fsr4: return L"FSR 4.1";
    case Backend::Dlss: return L"DLSS / DLAA";
    default:            return L"Off";
    }
}

// Every knob the mod exposes. Values are stored in settings.ini so the file
// stays hand-editable; the in-game overlay writes the same keys.
struct Settings
{
    // [General]
    int backend    = int(Backend::Auto);
    int showStatus = 1;
    int debugMotion = 0;
    int traceOnly  = 0;
    int captureOnly = 0;
    int historyMB  = 128;
    int overlayKey = VK_F7;      // hotkey that toggles the configuration panel

    // [FSR] Upscaling ratio. 0 = Native AA (1.0x, the default and current
    // behaviour), 1 = Quality (1.5x), 2 = Balanced (1.7x), 3 = Performance (2.0x).
    // Only the upscaler's input resolution changes: the game still renders at full
    // resolution and the frame is downsampled on the way in, so this trades image
    // detail for upscaler cost without touching the game's render targets.
    int fsrQuality = 0;

    // [FSR]
    int   fsrSharpnessPercent = 0;   // 0..100 -> 0.0..1.0
    int   fsrNonLinear        = 0;   // FSR4 non-linear (gamma) colour space flag
    int   fsrMvScalePercent   = 100; // motion vector scale, 25..400 %
    int   fsrMvSignY          = 1;   // +1 / -1, motion vector Y convention
    int   fsrDebugView        = 0;   // ask the upscaler for its debug visualisation
    // Colour space of what is fed to the upscaler. 0 = detect from the game's
    // backbuffer format; the rest are manual overrides for HDR setups whose
    // container format does not match its transfer function.
    int   fsrColorSpace       = 0;   // 0 auto | 1 sRGB (SDR) | 2 PQ (HDR10) | 3 linear (scRGB)
    std::wstring fsrDllPath;         // explicit amd_fidelityfx_upscaler_dx12.dll override

    static const wchar_t* ColorSpaceName(int value)
    {
        switch (value)
        {
        case 1: return L"sRGB (SDR)";
        case 2: return L"PQ / HDR10";
        case 3: return L"线性 scRGB";
        default: return L"自动";
        }
    }

    // Bumped whenever a value actually changed, so consumers can react once.
    int revision = 0;

    bool operator==(const Settings& o) const
    {
        return backend == o.backend && showStatus == o.showStatus && debugMotion == o.debugMotion &&
               traceOnly == o.traceOnly && captureOnly == o.captureOnly && historyMB == o.historyMB &&
               overlayKey == o.overlayKey && fsrSharpnessPercent == o.fsrSharpnessPercent &&
               fsrNonLinear == o.fsrNonLinear && fsrMvScalePercent == o.fsrMvScalePercent &&
               fsrMvSignY == o.fsrMvSignY && fsrDebugView == o.fsrDebugView && fsrDllPath == o.fsrDllPath &&
               fsrColorSpace == o.fsrColorSpace;
    }

    static std::wstring IniPath() { return ModDirectory() + L"\\settings.ini"; }

    static int ReadInt(const wchar_t* section, const wchar_t* key, int fallback)
    {
        return int(GetPrivateProfileIntW(section, key, UINT(fallback), IniPath().c_str()));
    }

    static std::wstring ReadString(const wchar_t* section, const wchar_t* key)
    {
        wchar_t buffer[MAX_PATH * 4]{};
        GetPrivateProfileStringW(section, key, L"", buffer, ARRAYSIZE(buffer), IniPath().c_str());
        return buffer;
    }

    static void WriteInt(const wchar_t* section, const wchar_t* key, int value)
    {
        wchar_t text[32]{};
        swprintf_s(text, L"%d", value);
        WritePrivateProfileStringW(section, key, text, IniPath().c_str());
    }

    static void WriteString(const wchar_t* section, const wchar_t* key, const std::wstring& value)
    {
        WritePrivateProfileStringW(section, key, value.c_str(), IniPath().c_str());
    }

    // Reads the ini, and only reports a change when something really moved.
    bool Load()
    {
        Settings next;
        next.backend     = ReadInt(L"General", L"Backend", int(Backend::Auto));
        next.showStatus  = ReadInt(L"General", L"ShowStatus", 1);
        next.debugMotion = ReadInt(L"General", L"DebugMotion", ReadInt(L"DLSS", L"DebugMotion", 0));
        next.traceOnly   = ReadInt(L"General", L"TraceOnly", ReadInt(L"DLSS", L"TraceOnly", 0));
        next.captureOnly = ReadInt(L"General", L"CaptureOnly", ReadInt(L"DLSS", L"CaptureOnly", 0));
        next.historyMB   = ReadInt(L"General", L"HistoryMB", ReadInt(L"DLSS", L"HistoryMB", 128));
        next.overlayKey  = ReadInt(L"General", L"OverlayKey", VK_F7);
        next.fsrQuality  = ReadInt(L"FSR", L"Quality", 0);

        next.fsrSharpnessPercent = ReadInt(L"FSR", L"Sharpness", 0);
        next.fsrNonLinear        = ReadInt(L"FSR", L"NonLinearColorSpace", 0);
        next.fsrMvScalePercent   = ReadInt(L"FSR", L"MotionVectorScale", 100);
        next.fsrMvSignY          = ReadInt(L"FSR", L"MotionVectorSignY", 1);
        next.fsrDebugView        = ReadInt(L"FSR", L"DebugView", 0);
        next.fsrColorSpace       = ReadInt(L"FSR", L"ColorSpace", 0);
        next.fsrDllPath          = ReadString(L"FSR", L"UpscalerDll");

        // Backwards compatibility with the pre-FSR4 ini, which only had Enable.
        if (ReadInt(L"General", L"Backend", -1) < 0)
        {
            if (ReadInt(L"DLSS", L"Enable", 1) == 0) next.backend = int(Backend::Off);
        }

        next.backend     = std::clamp(next.backend, 0, 3);
        next.historyMB   = int(std::clamp(next.historyMB, 32, 512));
        next.fsrSharpnessPercent = std::clamp(next.fsrSharpnessPercent, 0, 100);
        next.fsrMvScalePercent   = std::clamp(next.fsrMvScalePercent, 25, 400);
        next.fsrMvSignY  = next.fsrMvSignY < 0 ? -1 : 1;
        next.showStatus  = next.showStatus ? 1 : 0;
        next.debugMotion = next.debugMotion ? 1 : 0;
        next.traceOnly   = next.traceOnly ? 1 : 0;
        next.captureOnly = next.captureOnly ? 1 : 0;
        next.fsrNonLinear = next.fsrNonLinear ? 1 : 0;
        next.fsrDebugView = next.fsrDebugView ? 1 : 0;
        next.fsrColorSpace = std::clamp(next.fsrColorSpace,0,3);
        next.fsrQuality = std::clamp(next.fsrQuality,0,3);

        if (next == *this) return false;
        int previousRevision = revision;
        static_cast<Settings&>(*this) = next;
        revision = previousRevision + 1;
        return true;
    }

    void Save() const
    {
        WriteInt(L"General", L"Backend", backend);
        WriteInt(L"General", L"ShowStatus", showStatus);
        WriteInt(L"General", L"DebugMotion", debugMotion);
        WriteInt(L"General", L"TraceOnly", traceOnly);
        WriteInt(L"General", L"CaptureOnly", captureOnly);
        WriteInt(L"General", L"HistoryMB", historyMB);
        WriteInt(L"General", L"OverlayKey", overlayKey);
        WriteInt(L"FSR", L"Quality", fsrQuality);
        WriteInt(L"FSR", L"Sharpness", fsrSharpnessPercent);
        WriteInt(L"FSR", L"NonLinearColorSpace", fsrNonLinear);
        WriteInt(L"FSR", L"MotionVectorScale", fsrMvScalePercent);
        WriteInt(L"FSR", L"MotionVectorSignY", fsrMvSignY);
        WriteInt(L"FSR", L"DebugView", fsrDebugView);
        WriteInt(L"FSR", L"ColorSpace", fsrColorSpace);
        WriteString(L"FSR", L"UpscalerDll", fsrDllPath);
        // Keep the legacy key in sync so older builds and docs stay truthful.
        WriteInt(L"DLSS", L"Enable", backend == int(Backend::Off) ? 0 : 1);
    }
};
