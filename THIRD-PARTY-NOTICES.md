# Third-party notices

The MIT license at the repository root applies to this project's own code and documentation. Third-party components retain their own licenses, reproduced or referenced below.

## MinHook

[Minhook](https://github.com/TsudaKageyu/minhook): BSD-style license; see `third_party/minhook/LICENSE.txt` and `licenses/MinHook-LICENSE.txt`. The included disassembler carries its original notices.

## NVIDIA DLSS / NGX SDK

[NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS): NVIDIA RTX SDKs License, in `licenses/NVIDIA-RTX-SDK-LICENSE.txt`. The mod links the SDK's static library. SDK headers and libraries are **not included in this repository and are not downloaded by it**; obtain them from the upstream repository for local builds. The release does not include `nvngx_dlss.dll` — obtain the runtime separately from NVIDIA and follow its license.

This software contains source code provided by NVIDIA Corporation.

## AMD FidelityFX SDK (FSR 4.1)

Public API headers from the [FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) v2.3.0 are vendored under `third_party/ffx/`:

| Vendored | Upstream path |
| --- | --- |
| `third_party/ffx/api/include/ffx_api.h` | `Kits\FidelityFX\api\include\ffx_api.h` |
| `third_party/ffx/api/include/ffx_api_types.h` | `Kits\FidelityFX\api\include\ffx_api_types.h` |
| `third_party/ffx/api/include/ffx_api_loader.h` | `Kits\FidelityFX\api\include\ffx_api_loader.h` |
| `third_party/ffx/api/include/dx12/ffx_api_dx12.h` | `Kits\FidelityFX\api\include\dx12\ffx_api_dx12.h` |
| `third_party/ffx/upscalers/include/ffx_upscale.h` | `Kits\FidelityFX\upscalers\include\ffx_upscale.h` |

Every one of those files appears on the MIT exception list in the SDK's `docs/license.md`. That document is kept verbatim at `third_party/ffx/LICENSE.txt`, and the files on its exception list are subject to the following license:

```
Copyright (C) Advanced Micro Devices, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy of this
software and associated documentation files (the "Software"), to deal in the Software
without restriction, including without limitation the rights to use, copy, modify,
merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to the following
conditions:

The above copyright notice and this permission notice shall be included in all copies
or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE
OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

`Kits\FidelityFX\signedbin\amd_fidelityfx_upscaler_dx12.dll` is on the same exception list, so shipping it with this notice would be permitted. **This project does not ship it, and does not download it.** The mod loads a copy that the user has obtained and placed themselves, discovers one already installed by an FSR 4 game, or does without. See the README for where to put it.

The remainder of the SDK (everything not on the exception list) is covered by AMD's own FidelityFX license, which permits redistribution **in binary form only** and prohibits reverse engineering. No such files are included here.

## Trademarks and affiliation

AMD, Radeon, FidelityFX and FSR are trademarks or registered trademarks of Advanced Micro Devices, Inc. NVIDIA, GeForce, RTX and DLSS are trademarks or registered trademarks of NVIDIA Corporation. Hatsune Miku and Project DIVA are trademarks of their respective owners. ReShade is a project of its own authors.

This is an unofficial, fan-made mod. It is **not affiliated with, authorised, endorsed or sponsored by** AMD, NVIDIA, SEGA, Crypton Future Media or the developers or publishers of Hatsune Miku: Project DIVA Mega Mix+. All trademarks remain the property of their respective owners.

No game executables, game shaders, or commercial ReShade add-ons are distributed here. ReShade itself is not bundled, and the optional configuration add-on in this repository communicates with ReShade only through the documented add-on API.
