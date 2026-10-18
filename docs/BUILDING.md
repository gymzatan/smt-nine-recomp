# Building from your Japanese disc

## Requirements

- Windows 10/11 x64 and sufficient free disk space for the extracted disc, generated C, native build, language packages and high-resolution font atlases. Keep at least 15 GB free; 16 GB RAM or more is recommended.
- Python 3.14 x64, available through the Windows `py` launcher.
- Visual Studio 2022 Build Tools with Desktop development with C++, Windows SDK, CMake and Ninja. The scripts locate the installation using `vswhere`; no developer drive paths are required. A portable installation can be selected explicitly with `NINE_VS_ROOT` set to its Build Tools directory.
- [Rustup](https://rust-lang.org/tools/install/) installed and on PATH. The default Windows installer adds `%USERPROFILE%\.cargo\bin` to the user PATH; fully close and reopen the terminal or editor afterward, then verify `rustup --version`. `setup.cmd` installs the pinned Rust toolchain, not Rustup itself.
- For `--with-chinese` only: FFmpeg and FFprobe on PATH, with WMV2 encoding and the subtitles/libass filter enabled.
- For `--with-chinese` only: Windows Simplified Chinese supplemental fonts, `%WINDIR%\Fonts\simsun.ttc` and `%WINDIR%\Fonts\simhei.ttf`. Font files are not included in this project. Chinese glyph identities, encodings, layout and rendering rules are supplied by the repository. Japanese builds and English imports do not require these fonts or FFmpeg/FFprobe.
- Internet access for the declared Python, Cargo, DXVK and WineD3D dependencies. Their origins and the GPU archive checksums are recorded in `requirements.txt`, `Cargo.lock` and `config/dependencies.json`.

## Supported input

Gameplay hardware requirements and the limits of current GPU/VRAM verification are listed in the [README](../README.md#system-requirements). The RAM/disk guidance above is for building.

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

The default pipeline extracts Japanese resources into `data/japanese`, prepares the Japanese runtime image and shared code-generation input, builds title assets, generates the game C from the pinned original lifter, applies the explicit project codegen edits, builds the DSP dependency, and compiles the existing host/runtime and Config. It does not create Chinese Media, high-resolution font atlases or subtitled movies.

To include Chinese, use this initial command instead:

```bat
build.cmd --iso "D:\Games\Shin Megami Tensei - Nine (Japan).iso" --with-chinese
```

This additionally creates `data/chinese/Media`, `runtime/fonts` and the Chinese runtime image through the existing text/font/subtitle writers. Both builds retain the same checked shared code-generation recipe, including its existing popup edits. In a Japanese-only build, the prepared code-generation image stays under `build/localization/native_codegen.xbe` and is not installed as a playable Chinese image. The source translation inputs used by that recipe remain in the repository; no Chinese system fonts or movie tools are needed for this step. The build does not use any development-directory XBE, generated C, Media, prebuilt DSP library, font atlas or game executable.

The native game and imported languages use the accepted `Release` compiler configuration. The resulting entry points are `Config.exe` and `smt9_recomp.exe` in the project root. Double-click either executable directly. The game opens without a console window. Runtime paths are resolved from the executable directory; keep `data`, `runtime` and `backends` beside the executables when moving the folder. No drive letter or launcher script is required. Defaults are an English interface and Japanese game; `--with-chinese` selects Chinese defaults instead. Existing saved settings remain in effect.

The default `Auto` graphics selection attempts Vulkan, OpenGL, then system D3D11. Each implementation checks its own driver requirements. A failed DLL load, factory creation or device creation advances to the next backend. Partial device objects are released immediately; runtime DLLs load once and remain until process exit because unloading them can invalidate shared native-driver state even after another device is created. A manually selected backend is used exclusively. Errors after successful device initialization still stop explicitly; a driver crash or hang cannot return an initialization error to this loop.

Direct GUI launches write diagnostics to `runtime/startup.log`, including each graphics attempt, failure code and selected adapter/backend. If graphics initialization fails, an error dialog points to that log and Config. Launches with redirected standard error retain their existing output destination.

OpenGL requests a 1 ms Windows timer period after device creation and releases the request at process exit. WineD3D polls completion events on a command thread; coarse host waits can stall the original per-glyph fences even at 1x resolution. GPU completion checks, game clocks, the selected 30/60 Hz rate, glyph rendering and other backends retain their existing behavior.

Chinese high-resolution font atlases use read-only file mappings. The existing texture cache reads the original pixels when a page is needed, without allocating a private copy of every atlas at startup. Keep `runtime/fonts` beside the executable when Chinese is included. Japanese and imported English use their own original font assets. This changes source memory use, not glyphs, layout, resolution or GPU texture sizes.

If a build is interrupted, keep the unchanged checkout and completed outputs, then use:

```bat
build.cmd --iso "D:\Games\Shin Megami Tensei - Nine (Japan).iso" --resume
```

Include `--with-chinese` again when resuming a Chinese-enabled build. The language selection is recorded in `build/progress.json`; changing it during resume fails explicitly. Completed phases are reused only after their recorded output hashes match. A failed phase is not marked complete. Do not use this continuation option to reuse outputs after changing source or translation inputs, or to resume a build created before this selection was recorded. Use a fresh checkout to change the initial language selection. For development after a successful build, regenerate affected resources/code explicitly and use CMake to rebuild the affected targets.

`build/completion.json` records the actual disc, tool paths, duration and executable identities. Build completion is separate from game-flow validation.

## Languages

Config lists Japanese after the default build, and additionally Chinese with `--with-chinese`. Interface language (Chinese/Japanese/English) and game language are independent. Selecting an unavailable Chinese game through a manually edited INI fails explicitly; it does not change the selected language automatically.

For an external English translation, first apply that translation's patch to your own ISO. Open Config and choose **Import or update language ISO…**. Use identifier `English`, display name `English`, and select the already-patched ISO. The importer extracts its complete assets and recompiles only the changed functions using the existing local generated baseline. A successful build atomically registers a separate language package; an unsuccessful update leaves the previously registered package intact.

The initial Japanese build is required before language import; Chinese is not required. Imported executables inherit `SMT9_WITH_CHINESE` from the base build's existing CMake cache. The project does not download, distribute or apply the English translation patch. A translation that changes the unsupported executable layout fails explicitly and requires integration of its changes.

## Local files

`runtime/NINE.ini` stores configuration. Imported packages and their logs live under `runtime/languages`. Screenshots are under `runtime/Screenshots`. Ordinary user data is shared by the selected languages through the existing host path mapping; imported TitleData/cache and instant states are isolated by package. Instant states are valid only for the exact executable/image that created them. Do not commit or distribute any of these files.

Ordinary NINE saves accessed through the Xbox Partition1 path live under `data/chinese/UDATA`, including Japanese-only builds and imported languages. Startup creates the shared root as an ordinary directory when needed. This existing directory name is retained for save compatibility; it does not require Chinese Media or enable Chinese game support. `runtime/save` holds the runtime's other mapped user/cache directories. Preserve your ordinary saves separately when moving between checkouts.

PC storage uses ordinary directories. Utility-drive startup selects the existing cache directory, validates it through the existing volume API and registers the original drive alias. It does not read or format raw Xbox partitions and does not create `Partition*.img` files. Ordinary saves keep the same paths; imported languages retain their existing separate cache directories.
