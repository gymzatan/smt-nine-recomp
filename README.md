# SMT NINE Recomp

A native Windows port of **Shin Megami Tensei NINE** for the original Xbox, built with [xboxrecomp](https://github.com/sp00nznet/xboxrecomp). Game code is statically translated from your Japanese retail disc and compiled locally. Japanese and the project's Chinese translation are built together. Config can import an ISO that you have separately patched with an English translation.

This repository contains project source, configuration, patch rules and Chinese translation inputs. Supply your own supported Japanese ISO. Game executables, disc assets, generated game C, Microsoft fonts, saves and external translations are excluded.

[中文说明](README.zh-CN.md) · [Build instructions](docs/BUILDING.md) · [Controls](docs/CONTROLS.md) · [Status and limits](docs/STATUS.md) · [Build validation](docs/VALIDATION.md) · [Third-party notices](THIRD_PARTY.md)

## Build and run

Requires Windows x64, Python 3.14, Visual Studio 2022 C++ Build Tools including Windows SDK/CMake/Ninja, Rustup, FFmpeg/FFprobe on PATH, and Windows Simplified Chinese supplemental fonts (SimSun and SimHei). See the build guide for the supported disc identity and tool setup.

```bat
git clone https://github.com/gymzatan/smt-nine-recomp.git
cd smt-nine-recomp
setup.cmd
build.cmd --iso "D:\Games\Shin Megami Tensei - Nine (Japan).iso"
bin\Config.exe
run.cmd
```

The build extracts and verifies the ISO, generates Chinese resources and subtitles through the existing writers, prepares native images, translates game functions with the pinned lifter, builds the DSP/runtime and compiles the game and Config. Original movie audio is retained. No image-generation or translation service is called during a build.

In Config, interface language and game language are independent. Choose Japanese or Chinese for the built-in game. To add English, use **Import or update language ISO…**, enter `English` as its identifier and display name, then select your already-patched ISO. Import compiles a separate language package; it does not include or apply an English translation patch for you.

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
| `data/`, `runtime/`, `bin/`, `build/` | Ignored local inputs and generated outputs |

## Project status

The current port has been exercised through startup, ordinary saving/loading, movies, street navigation, shops, story battles, RTS tutorials and chapter two entry. Display options include multiple aspect ratios, resolution scales, 30/60 Hz, and D3D11/Vulkan/OpenGL selection. Exact-executable instant checkpoints, pause and screenshots are supported. This initial source release is not a full-playthrough or stable-60-FPS guarantee; specific remaining coverage is listed in the status document.

## License and credits

Project-authored code and Chinese translation inputs are provided under the MIT license. Third-party components retain their own licenses, including LGPL portions of xboxrecomp/WineD3D; see `THIRD_PARTY.md` and the retained notices. SimSun and SimHei are read from the user's Windows installation and are not redistributed. Shin Megami Tensei NINE and its original assets belong to their respective rights holders. This is an independent fan project.
