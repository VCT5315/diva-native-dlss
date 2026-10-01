# Diva Native DLSS

为 **Hatsune Miku: Project DIVA Mega Mix+** 添加原生分辨率 DLAA 的实验性模组。当前版本 **0.2.4**，已用 **DMM PR56** 测试。

输入与输出分辨率相同，采用 DLAA 模式。运动信息通过游戏实际绘制的几何重建，包含摄像机、模型和骨骼变化；不依赖游戏原有 TAA，也不使用屏幕图像估算的光流。

[下载安装包](https://github.com/xureimu-bit/diva-native-dlss/releases) · [可调物理模组](https://github.com/xureimu-bit/diva-physics-timing)

## 安装

需要 Windows x64、支持 DLSS 的 NVIDIA RTX 显卡与驱动，以及可加载 DLL 模组的 DivaModLoader / DMM。测试环境为 Direct3D 11。

1. 完全退出游戏，下载 Release 中的 `DivaNativeDLSS-0.2.4.zip`，解压到游戏根目录。最终应有 `mods/Diva Native DLSS/DivaNativeDLSS.dll`。
2. 从 [NVIDIA 官方 DLSS 仓库](https://github.com/NVIDIA/DLSS) 单独获取 `nvngx_dlss.dll`，放在游戏根目录，也就是游戏 EXE 所在目录。安装包不包含 NVIDIA 运行库。
3. 在 DMM 中勾选 **Diva Native DLSS**，通过 DMM 启动。先进入一个 3D PV 确认效果。
4. 更新时先备份自己的 `settings.ini`。卸载可在 DMM 中取消勾选，或退出游戏后删除该模组文件夹。

普通 DLAA 不需要 ReShade。本模组本身不提供神经渲染；额外的神经渲染需要兼容的桥接器与插件，此 Release 不包含它们。

## 游戏内按键

| 按键 | 作用 |
| --- | --- |
| F4 | 切换 DLAA，并显示短暂的开关提示；长按只切换一次 |
| F5 | 运动矢量可视化；正常游玩时保持关闭 |
| F6 | 重置时间历史 |

快捷键可能与其他模组重叠。`mods/Diva Native DLSS/settings.ini` 每隔一段时间重新读取，可在运行中修改：

```ini
[DLSS]
Enable=1
DebugMotion=0
TraceOnly=0
CaptureOnly=0
ShowStatus=1
HistoryMB=128
```

`Enable` 为开关，`ShowStatus` 控制提示；`TraceOnly`、`CaptureOnly` 为诊断选项，正常使用保持 0。日志输出到模组目录的 `DivaNativeDLSS.log`，运行库缓存位于 `ngx-cache`。

## 实现与范围

- 重放受支持的顶点着色器，通过 Stream Output 取得游戏变换后的几何，再将前后帧顶点对应生成深度和运动矢量。
- 使用子像素抖动及同分辨率 NGX SuperSampling / DLAA 输出；不提供降低内部渲染分辨率或帧生成。
- 在 GPU 的 `pass_sprite` 事件边界提交场景处理，使后续歌词和 UI 绘制继续使用原始像素。0.2.4 包含该提交边界修正。
- 暂停时尝试保留最近的处理画面，并保留变化的菜单/UI；明显场景变化会放弃旧结果。

这是实验版本。支持范围主要是匹配显示尺寸的单采样场景、常见三角形绘制与有限实例绘制。间接绘制、特殊几何/曲面着色器、部分透明效果、粒子及切镜头仍可能有重建缺失或拖影。不同 PV、分辨率和其他渲染模组组合需要自行验证。DLAA 会增加 GPU 开销。

如果没有效果，检查日志中的 NGX 初始化、DLSS 创建和求值结果。缺少运行库、硬件不支持或求值失败时，不应把可视化轮廓误认为 DLAA 已成功运行。报告问题时请提供游戏版本、GPU/驱动、PV 名称、开关对比与相关日志片段，上传前删掉个人路径。

## 从源码构建

需要 Visual Studio 2022 的 C++ 桌面开发工具、Windows SDK、CMake 3.20+。在仓库目录执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\FetchSDK.ps1
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

`FetchSDK.ps1` 从固定 NVIDIA 提交下载 SDK 头文件、静态库和许可证，并校验 SHA-256。请阅读 NVIDIA 的许可条款。下载的 SDK 不纳入 Git。

输出为 `build/Release/DivaNativeDLSS.dll`。发布包中的 DLL 是已做游戏内验证的构建；重新构建不保证二进制逐字节一致。

同时生成 D3D11 GPU 验证程序 `gpu_test.exe`。在独立测试目录放置 DLL、`settings.ini` 和 `nvngx_dlss.dll`，将 `ShowStatus=0`，然后执行：

```powershell
.\gpu_test.exe sr-uidepth-pause-keys .\DivaNativeDLSS.dll
```

该验证覆盖场景处理、UI 保留、暂停和短按/长按逻辑，需要真实 GPU。它会更改测试目录中的配置，请不要在游戏目录运行。

## 许可

项目自己的代码采用 [MIT](LICENSE)。MinHook、NVIDIA SDK 及运行库保留各自条款，见 [第三方声明](THIRD-PARTY-NOTICES.md)。游戏及 NVIDIA 商标属于其各自权利人，本项目为非官方模组。

## English

Experimental full-resolution DLAA for Project DIVA Mega Mix+, tested with DMM PR56. Motion vectors are reconstructed from the game's transformed geometry rather than image optical flow. The game does not need built-in TAA. Copy the release into the game directory and separately obtain `nvngx_dlss.dll` from NVIDIA. F4 toggles DLAA, F5 visualizes motion vectors, F6 resets history. ReShade is optional; neural rendering and frame generation are not included. See the third-party notices for NVIDIA's separate SDK/runtime terms.
