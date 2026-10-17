# Building from your Japanese disc

## Requirements

- Windows 10/11 x64 and sufficient free disk space for the extracted disc, generated C, native build, language packages and high-resolution font atlases. Keep at least 15 GB free; 16 GB RAM or more is recommended.
- Python 3.14 x64, available through the Windows `py` launcher.
- Visual Studio 2022 Build Tools with Desktop development with C++, Windows SDK, CMake and Ninja. The scripts locate the installation using `vswhere`; no developer drive paths are required. A portable installation can be selected explicitly with `NINE_VS_ROOT` set to its Build Tools directory.
- Rustup on PATH. `setup.cmd` installs the pinned Rust toolchain specified by the DSP source.
- FFmpeg and FFprobe on PATH, with WMV2 encoding and the subtitles/libass filter enabled.
- Windows Simplified Chinese supplemental fonts: `%WINDIR%\Fonts\simsun.ttc` and `%WINDIR%\Fonts\simhei.ttf`. The build reads these local fonts. Font files are not included in this project. Chinese glyph identities, encodings, layout and rendering rules are supplied by the repository.
- Internet access for the declared Python, Cargo, DXVK and WineD3D dependencies. Their origins and the GPU archive checksums are recorded in `requirements.txt`, `Cargo.lock` and `config/dependencies.json`.

## Supported input

The supported unmodified Japanese ISO is 2,188,050,432 bytes with SHA-256:

```text
eafcd00e7fbc53bb10a0ac5743faf2d5ea3e1be9f7351ddb3e97b3c8d916d446
```

Its `default.xbe` SHA-256 is `6c249401369ed41c2a319762f0b56b80aefde3efe766ac0d8731889a4150deac`. The complete 3,515-file identity list is in `config/retail.json`. Supply this disc image yourself. Other dumps or modified images are rejected by the initial build; externally translated images use the separate Config import flow.

## Build

In a fresh checkout:

```bat
setup.cmd
build.cmd --iso "D:\Games\Shin Megami Tensei - Nine (Japan).iso"
```

`--jobs 4` is the default compiler parallelism. Resource preparation is sequential. CMake/Ninja and Cargo manage the dependency graph for compilation; object and crate outputs are independent.

The pipeline extracts Japanese resources into `data/japanese`, creates Chinese resources in `data/chinese`, prepares both native XBE images, builds title/font/subtitle assets, generates the game C from the pinned original lifter, applies the explicit project codegen edits, builds the DSP dependency, and compiles the existing host/runtime and Config. The build does not use any development-directory XBE, generated C, Media, prebuilt DSP library, font atlas or game executable.

The native game and imported languages use the accepted `Release` compiler configuration. The resulting entry points are `Config.exe` and `smt9_recomp.exe` in the project root. Double-click either executable directly. The game opens without a console window. Runtime paths are resolved from the executable directory; keep `data`, `runtime` and `backends` beside the executables when moving the folder. No drive letter or launcher script is required. Config defaults to a Chinese interface and Chinese game language.

If a build is interrupted, keep the unchanged checkout and completed outputs, then use:

```bat
build.cmd --iso "D:\Games\Shin Megami Tensei - Nine (Japan).iso" --resume
```

Completed phases in `build/progress.json` are reused only after their recorded output hashes match. A failed phase is not marked complete. Do not use this continuation option to reuse outputs after changing source or translation inputs. For development after a successful build, regenerate affected resources/code explicitly and use CMake to rebuild the affected targets.

`build/completion.json` records the actual disc, tool paths, duration and executable identities. Build completion is separate from game-flow validation.

## Languages

Config lists Japanese and Chinese after the initial build. Interface language (Chinese/Japanese/English) and game language are independent.

For an external English translation, first apply that translation's patch to your own ISO. Open Config and choose **Import or update language ISO…**. Use identifier `English`, display name `English`, and select the already-patched ISO. The importer extracts its complete assets and recompiles only the changed functions using the existing local generated baseline. A successful build atomically registers a separate language package; an unsuccessful update leaves the previously registered package intact.

The initial Japanese build is required before language import. The project does not download, distribute or apply the English translation patch. A translation that changes the unsupported executable layout fails explicitly and requires integration of its changes.

## Local files

`runtime/NINE.ini` stores configuration. Imported packages and their logs live under `runtime/languages`. Screenshots are under `runtime/Screenshots`. Ordinary user data is shared by the selected languages through the existing host path mapping; imported TitleData/cache and instant states are isolated by package. Instant states are valid only for the exact executable/image that created them. Do not commit or distribute any of these files.

Ordinary NINE saves accessed through the Xbox Partition1 path live under `data/chinese/UDATA`, including when Japanese or an imported language supplies the selected Media. `runtime/save` holds the runtime's other mapped user/cache directories. Preserve your ordinary saves separately when moving between checkouts.
