#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <algorithm>
#include <cstdio>
#include <cstdarg>
#include <cmath>
using Microsoft::WRL::ComPtr;

// The NVIDIA NGX (DLSS) backend is optional: it needs the SDK import library
// that FetchSDK.ps1 downloads. Without it the mod still builds and runs with
// the AMD FidelityFX (FSR4) backend alone, which is what an AMD GPU needs.
#ifndef DIVA_ENABLE_NGX
#define DIVA_ENABLE_NGX 0
#endif

#if DIVA_ENABLE_NGX
#include "../third_party/ngx/include/nvsdk_ngx.h"
#endif

void Log(const char* format,...);
std::wstring ModDirectory();
std::wstring GameDirectory();

// Header layering (each one includes the previous):
//   common.hpp        - platform headers, logging
//   settings.hpp      - Backend enum, Settings, settings.ini load/save
//   upscaler.hpp      - IUpscaler, the backend-neutral interface
//   upscaler_ngx.hpp  - NVIDIA DLSS / DLAA   (requires DIVA_ENABLE_NGX)
//   dx12_bridge.hpp   - D3D11 <-> D3D12 shared textures and fences
//   upscaler_fsr.hpp  - AMD FSR 4.1 through the FidelityFX API
//   overlay.hpp       - the in-game hot-configuration panel
//   render.hpp        - geometry capture and frame processing
