# Third-party notices

The MIT license at the repository root applies to this project's own code and documentation. Third-party components retain their own licenses.

- [MinHook](https://github.com/TsudaKageyu/minhook): BSD-style license; see `third_party/minhook/LICENSE.txt` and `licenses/MinHook-LICENSE.txt`. The included disassembler carries its original notices.
- [NVIDIA DLSS/NGX SDK](https://github.com/NVIDIA/DLSS): NVIDIA RTX SDKs License, in `licenses/NVIDIA-RTX-SDK-LICENSE.txt`. The mod links the SDK's static library. SDK headers/libraries are not included in this repository and are not downloaded by it; obtain them from the upstream repository for local builds. The release does not include `nvngx_dlss.dll`; obtain the runtime separately from NVIDIA and follow its license.
- [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) (FSR 4.1): the public API headers vendored under `third_party/ffx/` are MIT licensed. The `amd_fidelityfx_upscaler_dx12.dll` runtime is not part of this repository - the mod never downloads or redistributes it; it only locates a copy the user has installed. The license text is kept at `third_party/ffx/LICENSE.txt`. Per that file's `docs/license.md`, the headers and `amd_fidelityfx_upscaler_dx12.dll` are on the MIT exception list, so redistributing them alongside this mod is permitted provided AMD's copyright and permission notice are reproduced.

This software contains source code provided by NVIDIA Corporation.

No game executables, shaders extracted from the game, or commercial ReShade addons are distributed here.
