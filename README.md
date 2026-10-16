# Diva Native DLSS

为 **Hatsune Miku: Project DIVA Mega Mix+** 添加原生分辨率超分的实验性模组，支持**可切换的 NVIDIA DLSS/DLAA 与 AMD FSR 4.1 (Native AA)**。当前版本 **0.3.0**，已用 **DMM PR56** 测试。

输入与输出分辨率相同（DLAA / FSR Native AA）。运动信息通过游戏实际绘制的几何重建，包含摄像机、模型和骨骼变化；不依赖游戏原有 TAA，也不使用屏幕图像估算的光流。同一套几何重建管线同时喂给两个后端，因此可以在运行时切换。

> **AMD 显卡**：FSR 4.1 需要 RDNA4（RX 9000 系列）。本项目已在 **RX 9070 XT**（驱动 32.0.32015.2008）+ 真实 FSR 4.1 运行库上端到端验证，覆盖 **3840×2160** 与 **1920×1080**。实现细节、实测数据与限制见 [docs/fsr4.md](docs/fsr4.md)。

> 本项目是 [xureimu-bit/diva-native-dlss](https://github.com/xureimu-bit/diva-native-dlss) 的 fork：上游提供基线实现，本分支在此之上加入 ReShade 配置面板、**1920×1080 修复**与**切换设置掉驱动修复**。发布包请从**本 fork 的 Releases** 获取——上游的 Release 不包含这些改动。相关模组：[可调物理模组](https://github.com/xureimu-bit/diva-physics-timing)。

## 本版要点

**配置界面改为 ReShade overlay 附加组件。** 早期版本尝试在模组内自绘配置面板（F7），该面板在游戏的实际后缓冲上始终无法显示，已放弃。现在配置面板是一个独立的 ReShade 附加组件，使用 ReShade 自带的 ImGui 界面，两者只通过 `settings.ini` 耦合。

**修复 1920×1080 下模组完全失效。** 游戏在 1080p 下把视口与深度缓冲填充到 1088 行（1080 不是 16 的倍数），而模组内部三处尺寸判定用的是精确相等，于是每一次绘制都被拒绝，模组静默什么都不做。4K 下 2160 本身就是 16 的倍数，所以从未暴露。现已改为"覆盖屏幕且在容差内"。

**修复切换设置时掉驱动。** 关闭超分、切换色彩空间、切换调试视图都会销毁并重建 FSR 上下文与共享曲面，而此前没有等待 GPU 完成队列。D3D12 侧仍在使用这些资源时释放它们会导致设备移除。现在三条重建路径（`Invalidate` / `Shutdown` / `Ready`）都会先清空队列。

## 安装

需要 Windows x64 与可加载 DLL 模组的 DivaModLoader / DMM。测试环境为 Direct3D 11。

- **NVIDIA**：需要支持 DLSS 的 RTX 显卡与驱动。
- **AMD**：需要 RDNA4（RX 9000）与较新的驱动。

1. 完全退出游戏，下载 Release 中的 `DivaNativeDLSS-0.3.0.zip`，解压到游戏根目录。最终应有：
   - `mods/Diva Native DLSS/DivaNativeDLSS.dll` —— 模组本体
   - `DivaNativeDLSS.addon64` —— **放在游戏根目录**的 ReShade 配置面板（可选，见下）
2. 准备运行库（安装包都不包含）：
   - NVIDIA：从 [NVIDIA 官方 DLSS 仓库](https://github.com/NVIDIA/DLSS) 获取 `nvngx_dlss.dll`，放在游戏 EXE 所在目录。
   - AMD：需要 `amd_fidelityfx_upscaler_dx12.dll`，**请自行准备**——本模组不下载、也不分发任何运行库。把它放到 `mods/Diva Native DLSS/`、游戏根目录或 `游戏根目录/D3D12_Optiscaler/` 任一位置。模组也会扫描 Steam 库并选用版本最新的那一份，所以已装 FSR 4 的游戏（战地 6、黑神话悟空、赛博朋克 2077 等）自带的那份通常会被直接找到。
3. 在 DMM 中勾选 **Diva Native DLSS**，通过 DMM 启动。先进入一个 3D PV 确认效果。
4. 更新时先备份自己的 `settings.ini`。卸载可在 DMM 中取消勾选，或退出游戏后删除该模组文件夹。

普通 DLAA / FSR Native AA 都不需要 ReShade。ReShade 附加组件只提供配置界面，**不装也能用**，此时改 `settings.ini`。本模组本身不提供神经渲染；额外的神经渲染需要兼容的桥接器与插件，此 Release 不包含它们。

### ReShade 配置面板（可选）

`DivaNativeDLSS.addon64` 放在**游戏根目录**（与 `dxgi.dll`、其他 `.addon` 同级）。ReShade 6.x 会自动扫描该目录并加载它。已验证 ReShade **6.6.2** 与 **6.8.0**。

- 按 ReShade 的 overlay 键（默认 `Home`），找到 **Diva Native DLSS** 标签页
- 面板读写的就是 `mods/Diva Native DLSS/settings.ini`，模组每隔一段时间热重载，**改动几帧内生效，无需重启游戏**
- 加载失败时看 `ReShade.log` 里的 `Registered add-on "Diva Native DLSS"`

## 游戏内按键

| 按键 | 作用 |
| --- | --- |
| F4 | 切换超分开/关；每次切换弹一次状态提示 |
| F5 | 运动矢量可视化；正常游玩时保持关闭 |
| F6 | 重置时间历史 |

F4 / F5 / F6 是**全局**的——即使配置面板开着也照常生效。启动后第一帧会**无条件弹一次提示**显示当前后端状态，避免"配置停在关闭档而屏幕上毫无提示"这类误判。

## 配置

`mods/Diva Native DLSS/settings.ini`：

```ini
[General]
Backend=0            ; 0=自动 1=FSR 4.1 2=DLSS/DLAA 3=关闭
ShowStatus=1
DebugMotion=0
TraceOnly=0          ; 诊断：只记录不处理
CaptureOnly=0        ; 诊断：只捕获不超分
HistoryMB=128
OverlayKey=118       ; 0x76 = F7（仅旧版自绘面板使用，现已无作用）

[FSR]
Sharpness=0              ; 0..100，FSR 的 RCAS 锐化
ColorSpace=0             ; 0=自动 1=sRGB 2=PQ/HDR10 3=线性 scRGB
MotionVectorScale=100    ; 百分比，主要调参项
MotionVectorSignY=1      ; 运动矢量 Y 方向约定
DebugView=0              ; FSR 自带的调试可视化
UpscalerDll=             ; 可选的显式 DLL 路径
```

`Backend` 为后端选择，`Auto` 会在找到 FSR 运行库时优先用 FSR 4.1，否则使用 DLSS。`TraceOnly`、`CaptureOnly` 为诊断选项，正常使用保持 0。日志输出到模组目录的 `DivaNativeDLSS.log`。

**色彩空间**：容器格式不能决定传递函数。本模组按后缓冲格式自动判定——`R16G16B16A16_FLOAT` 视为线性 scRGB（3），`R10G10B10A2` 视为 PQ/HDR10（2），`R8G8B8A8` 视为 sRGB（1）。**手动选错档会比自动档明显更差**：把线性 scRGB 当作 PQ 声明会压死全部高光。不确定时保持 `0`。

如果画面拖影明显，用配置面板调 `MotionVectorScale` / `MotionVectorSignY`。写 `[DLSS]` 段的老配置文件仍可读取，会自动迁移。

## 实现与范围

- 重放受支持的顶点着色器，通过 Stream Output 取得游戏变换后的几何，再将前后帧顶点对应生成深度和运动矢量。
- 同一套几何重建结果同时提供给两个后端，可运行时切换：
  - **NVIDIA**：NGX SuperSampling / DLAA，同分辨率。
  - **AMD**：FSR 4.1 Native AA。FSR 4 只有 D3D12 后端，因此模组在**同一块适配器**上另建 D3D12 设备，用共享纹理与共享栅栏桥接。详见 [docs/fsr4.md](docs/fsr4.md)。
- 使用子像素抖动（偏移视口的浮点原点）；不提供降低内部渲染分辨率或帧生成。
- 在 GPU 的 `pass_sprite` 事件边界提交场景处理，使后续歌词和 UI 绘制继续使用原始像素。
- 暂停时尝试保留最近的处理画面，并保留变化的菜单/UI；明显场景变化会放弃旧结果。

这是实验版本。支持范围主要是匹配显示尺寸的单采样场景、常见三角形绘制与有限实例绘制。间接绘制、特殊几何/曲面着色器、部分透明效果、粒子及切镜头仍可能有重建缺失或拖影。不同 PV、分辨率和其他渲染模组组合需要自行验证。DLAA / FSR 都会增加 GPU 开销。

### 已知限制

- **超分档位（Quality/Balanced/Performance）未实现。** 只有 Native AA 一档。曾实现"降采样后交给 FSR 升采样"的档位，能工作（实测 4K 下输入 2560×1440、输出 3840×2160），但引入画面裁切且切换时可能掉驱动，已停用。代码路径保留但不可达。**注意**：这类档位省的是 FSR 的开销，游戏本身仍按全分辨率光栅化；要同时降低游戏开销需要改游戏的渲染目标绑定，风险高得多。
- **游戏自带的 MLAA / FXAA 关不掉**，只能在两者间二选一。这意味着送进超分器的画面**始终已经被空间滤波处理过**，高频细节已被削弱一部分。跳过该 pass 的思路已调研（AA pass 的着色器已识别，位于每帧后处理链倒数第三位），但未实现。
- **旧版自绘配置面板（F7）不可用**，已被 ReShade 附加组件取代；`OverlayKey` 保留但无作用。
- **DLSS/NGX 后端未经编译与运行验证** —— 开发环境没有 NVIDIA SDK，构建时自动回退到仅 FSR。相关代码路径保留。
- F4/F5/F6 可能与其它模组重叠。

如果没有效果，检查日志中的后端选择、运行库加载、上下文创建和求值结果。**缺少运行库、硬件不支持或求值失败时，不应把可视化轮廓误认为超分已成功运行**（F5 只是运动矢量预览）。报告问题时请提供游戏版本、GPU/驱动、PV 名称、开关对比与相关日志片段，上传前删掉个人路径。

## 从源码构建

需要 Visual Studio 2022 的 C++ 桌面开发工具、Windows SDK、CMake 3.20+。在仓库目录执行：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# 生成可直接解压到游戏根目录的发布包
cmake --build build --config Release --target package
```

构建产物在 `build/Release/`：`DivaNativeDLSS.dll`、`DivaNativeDLSS.addon64`、`gpu_test.exe`。`package` 目标会把它们组装成

```
release/DivaNativeDLSS-0.3.0/
├── DivaNativeDLSS.addon64                    ← 解压到游戏根目录
└── mods/Diva Native DLSS/DivaNativeDLSS.dll  ← 解压到游戏根目录
```

这一层目录解压到游戏根目录即可，并打包为同名 zip。

`build/`、`release/`、`*.dll`、`*.exe` 都在 `.gitignore` 中——**编译产物不入库**。ReShade 附加组件是独立目标（`DivaNativeDLSSAddon`），可用 `--target DivaNativeDLSSAddon` 单独构建；它**不引入任何 ReShade 或 ImGui 头文件**，全部 ABI 在 `src_reshade/reshade_min.hpp` 里按源码核实后手工声明（原因见 [docs/reshade-addon.md](docs/reshade-addon.md)：master 的 `RESHADE_API_VERSION` 是 20 而 6.6.2 只接受 18；包含 `imgui.h` 会触发 ImGui 版本锁）。

NVIDIA 后端是可选的。若 `third_party/ngx/lib/nvsdk_ngx_s.lib` 不存在，配置阶段会退回**仅 FSR** 构建并给出提示。要启用 DLSS，请自行从 [NVIDIA DLSS 仓库](https://github.com/NVIDIA/DLSS) 获取 SDK，按 `third_party/ngx/` 的布局放好头文件与静态库——**本仓库不下载、也不包含 NVIDIA SDK**。

发布包中的 DLL 是已做游戏内验证的构建；重新构建不保证二进制逐字节一致。

## 验证

同时生成 D3D11 GPU 验证程序 `gpu_test.exe`。在独立测试目录放置 DLL 与 `settings.ini`，将 `ShowStatus=0`（**必须**，否则提示会覆盖测试用来验证 UI 保留的采样点），然后执行：

```powershell
.\gpu_test.exe sr-uidepth-pause-keys .\DivaNativeDLSS.dll   # 场景处理 / UI 保留 / 暂停 / 按键
```

该验证覆盖场景处理、UI 保留、暂停、短按/长按逻辑以及设置落盘，需要真实 GPU。它会更改测试目录中的配置，请不要在游戏目录运行。

`-DBUILD_TOOLS=ON` 还会构建互操作诊断工具（`tools/`），用于在没有游戏的情况下验证 D3D11↔D3D12 桥接与 FSR 派发：

```powershell
.\fsr_test.exe "G:\...\amd_fidelityfx_upscaler_dx12.dll"
.\bridge_final.exe legacy
```

## 许可

项目自己的代码采用 [MIT](LICENSE)。MinHook、NVIDIA SDK、AMD FidelityFX SDK 及各自运行库保留各自条款，见 [第三方声明](THIRD-PARTY-NOTICES.md)。游戏及各厂商商标属于其各自权利人，本项目为非官方模组。

## English

Experimental native-resolution upscaling for Project DIVA Mega Mix+, tested with DMM PR56, with **switchable NVIDIA DLSS/DLAA and AMD FSR 4.1 (Native AA)**. Motion vectors are reconstructed from the game's transformed geometry rather than image optical flow. The game does not need built-in TAA. Both backends consume the same reconstructed geometry, so they can be swapped at runtime.

The configuration UI is a **ReShade add-on** (`DivaNativeDLSS.addon64`, placed in the game root), not an in-game panel: an earlier attempt at drawing a panel on the game's own back buffer never appeared, so the UI moved to ReShade's overlay. The two sides are coupled only through `settings.ini`, which the mod hot-reloads. ReShade is optional - without it, edit the ini directly.

NVIDIA needs `nvngx_dlss.dll` from NVIDIA. AMD needs `amd_fidelityfx_upscaler_dx12.dll`, which you supply yourself - the mod neither downloads nor redistributes runtimes. Put it in `mods/Diva Native DLSS/`, the game root, or `D3D12_Optiscaler/`; the mod also scans Steam libraries, so a copy shipped by an installed FSR 4 game is usually found on its own. FSR 4.1 requires RDNA4.

F4 toggles upscaling, F5 visualizes motion vectors, F6 resets history. The FSR backend bridges D3D11 to a D3D12 device because FSR 4 has no DX11 provider - see [docs/fsr4.md](docs/fsr4.md) for the design and measured validation results.

Known limitations are listed above; in short, only Native AA is available (the upscaling quality tiers were implemented, worked, but caused image cropping and driver resets and are disabled), the game's own MLAA/FXAA cannot be turned off, and the NVIDIA backend has never been compiled here. See the third-party notices for NVIDIA's and AMD's separate SDK/runtime terms.
