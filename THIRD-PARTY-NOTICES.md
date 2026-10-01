# Third-party notices

The MIT license at the repository root applies to this project's own code and documentation. Third-party components retain their own licenses.

- [MinHook](https://github.com/TsudaKageyu/minhook): BSD-style license; see `third_party/minhook/LICENSE.txt` and `licenses/MinHook-LICENSE.txt`. The included disassembler carries its original notices.
- [NVIDIA DLSS/NGX SDK](https://github.com/NVIDIA/DLSS): NVIDIA RTX SDKs License, in `licenses/NVIDIA-RTX-SDK-LICENSE.txt`. The mod links the SDK's static library. SDK headers/libraries are fetched for local builds by `FetchSDK.ps1` from a pinned upstream revision and are not included in this repository. The release does not include `nvngx_dlss.dll`; obtain the runtime separately from NVIDIA and follow its license.

This software contains source code provided by NVIDIA Corporation.

No game executables, shaders extracted from the game, or commercial ReShade addons are distributed here.
