# SMT NINE Recomp

A native Windows port of **Shin Megami Tensei NINE** for the original Xbox, built with [xboxrecomp](https://github.com/sp00nznet/xboxrecomp). Game code is statically translated from your Japanese retail disc and compiled locally. The default build provides Japanese; the project's Chinese translation is optional. Config can import an ISO that you have separately patched with an English translation, with or without Chinese support.

This repository contains project source, configuration, patch rules and Chinese translation inputs. Supply your own supported Japanese ISO. Game executables, disc assets, generated game C, Microsoft fonts, saves and external translations are excluded.

[中文说明](README.zh-CN.md) · [Build instructions](docs/BUILDING.md) · [Controls](docs/CONTROLS.md) · [Status and limits](docs/STATUS.md) · [Build validation](docs/VALIDATION.md) · [Third-party notices](THIRD_PARTY.md)

## System requirements

| Component | Requirement / verified scope |
| --- | --- |
| Operating system | Windows 10/11 x64. |
| CPU | x86-64 processor. A minimum CPU model and runtime RAM requirement have not been established. |
| Graphics | The runtime requests Direct3D feature level **11_0**. D3D11 uses the Windows graphics driver; Vulkan and OpenGL additionally require a driver compatible with the bundled DXVK or WineD3D implementation. Supporting an API name alone does not guarantee correct rendering. |
| Graphics memory | **No certified minimum VRAM figure yet. 512 MiB configurations are not verified as supported.** On integrated GPUs, dedicated memory is only part of the available memory; the process budget and shared system memory also matter. |
| Initial settings | Start with **1× render scale, 4:3, 30 Hz**. Higher scales increase render-target memory; 4× has sixteen times the target pixels of 1×, not sixteen times the total application memory. |
| Local compilation | At least **15 GB free disk space** and **16 GB RAM recommended** for extraction and building. These are build recommendations, not a measured gameplay minimum. |

Local street samples with an AMD Ryzen 9 9950X and RTX 5090 exercised D3D11, Vulkan and OpenGL; an AMD Radeon Graphics integrated adapter was additionally checked with D3D11. Japanese 1×, 4:3, 30 Hz samples ran around 29–31 FPS without the reported horizontal character gaps. [Measured process-memory samples](docs/VALIDATION.md#process-graphics-memory-samples) are recorded separately. This is a tested reference, not a minimum specification or a full-game guarantee. The exact reported female-player and restaurant scenes remain unverified.

For graphics reports, send `runtime/startup.log` from the affected run, your driver version and a nearby ordinary save. The log records dedicated/shared capacity and, where the selected runtime supports DXGI memory queries, process `current_bytes` and `budget_bytes` for local/nonlocal segments every five seconds while frames are presented. Unsupported queries are explicitly marked unavailable. Capacity, process usage and budget are different measurements; 512 MiB dedicated memory alone does not prove exhaustion. Translation layers can expose compatibility adapter names and capacity values, so include the GPU/driver information reported by Windows too. A new GUI launch replaces the log, so copy it before launching again.

## Build and run

Requires Windows x64, Python 3.14, Visual Studio 2022 C++ Build Tools including Windows SDK/CMake/Ninja, and Rustup. Only the optional Chinese build also requires FFmpeg/FFprobe on PATH and Windows Simplified Chinese supplemental fonts (SimSun and SimHei). See the build guide for the supported disc identity and tool setup.

```bat
git clone https://github.com/gymzatan/smt-nine-recomp.git
cd smt-nine-recomp
setup.cmd
build.cmd --iso "D:\Games\Shin Megami Tensei - Nine (Japan).iso"
Config.exe
smt9_recomp.exe
```

To include Chinese, add the option to the initial build command:

```bat
build.cmd --iso "D:\Games\Shin Megami Tensei - Nine (Japan).iso" --with-chinese
```

The build extracts and verifies the ISO, prepares native code and title resources, translates game functions with the pinned lifter, builds the DSP/runtime and compiles the game and Config. `--with-chinese` also builds Chinese scripts, fonts and subtitled movies through the existing writers. Without that option, those resource steps are skipped. Original movie audio is retained. No image-generation or translation service is called during a build.

The two executables are in the project root. Double-click `smt9_recomp.exe` to play and `Config.exe` for settings; no command script is needed. Runtime paths follow the executable, so the folder can be moved to another drive while keeping its resources together. The default build starts with English settings and Japanese game text; a Chinese-enabled build starts with Chinese settings and game text. Existing saved settings remain in effect.

In Config, interface language and game language are independent. Japanese is built in; Chinese appears only when enabled at build time. To play Japanese and English only, use the default build, then **Import or update language ISO…**, enter `English` as its identifier and display name, and select your already-patched ISO. Import compiles a separate language package; it does not include or apply an English translation patch for you.

## Repository layout

| Directory | Contents |
| --- | --- |
| `game/` | Native host, Config, game-specific overrides and tests |
| `vendor/xboxrecomp/` | Pinned recompiler/runtime with the project's accepted changes |
| `config/` | Function identities, ABI, dependency checksums and code-generation edits |
| `translations/zh-CN/` | Chinese text, encoding and original-reference identities |
| `font_pipeline/`, `localization/`, `writers/` | Existing font, subtitle and text builders |
| `patches/` | Rebuildable title corrections and staff-list edits |
| `reference/` | DSP and resampler dependency source |
| `tools/` | Disc-to-native-build integration |
| `data/`, `runtime/`, `backends/`, `build/` | Ignored local inputs and generated outputs |

## Project status

The current port has been exercised through startup, ordinary saving/loading, movies, street navigation, shops, story battles, RTS tutorials and chapter two entry. Display options include multiple aspect ratios, resolution scales, 30/60 Hz, and D3D11/Vulkan/OpenGL selection. Exact-executable instant checkpoints, pause and screenshots are supported. This initial source release is not a full-playthrough or stable-60-FPS guarantee; specific remaining coverage is listed in the status document.

## License and credits

Project-authored code and Chinese translation inputs are provided under the MIT license. Third-party components retain their own licenses, including LGPL portions of xboxrecomp/WineD3D; see `THIRD_PARTY.md` and the retained notices. SimSun and SimHei are read from the user's Windows installation and are not redistributed. Shin Megami Tensei NINE and its original assets belong to their respective rights holders. This is an independent fan project.
