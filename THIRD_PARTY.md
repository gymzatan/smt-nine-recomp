# Third-party source and dependencies

Project-authored code and Chinese translation inputs use the root MIT license. The components below retain their original notices and licenses.

| Component | Source and identity | Retained license |
| --- | --- | --- |
| xboxrecomp | [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp), base commit `193e2995ffaec871bb3be43a43bd66aa5fccfeb1`, with this project's runtime/lifter changes | `vendor/xboxrecomp/LICENSE`, `NOTICE`, `LICENSES/`; MIT with specified LGPL components |
| DSP56300 JIT | [mborgerson/dsp56300](https://github.com/mborgerson/dsp56300); vendored source and locked Cargo dependencies | `reference/dsp56300_release/dsp56300-0.1.3/LICENSE`; MIT and the individual Cargo crate licenses |
| libsamplerate | [libsndfile/libsamplerate](https://github.com/libsndfile/libsamplerate), commit `c96f5e3de9c4488f4e6c97f59f5245f22fda22f7` | `reference/libsamplerate/COPYING`; BSD 2-clause |
| Vulkan interface headers | [KhronosGroup/Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers/tree/vulkan-sdk-1.4.341.0) | SPDX notices retained in `vendor/xboxrecomp/src/d3d/vulkan/` and `vk_video/` |
| DXVK | [doitsujin/dxvk](https://github.com/doitsujin/dxvk); download declared in `config/dependencies.json` | `LICENSES/DXVK_LICENSE` |
| WineD3D for Windows | [adolfintel/wined3d4win](https://github.com/adolfintel/wined3d4win), [Wine source](https://github.com/wine-mirror/wine); download declared in `config/dependencies.json` | `LICENSES/WINED3D_COPYING.LIB` |

GPU dependencies are downloaded into ignored local build directories. Preparation validates archive contents and x64 DLL structure, then uses the PE import table to locate and change the eight-byte DXGI import module names in their D3D11 DLLs to `nvgi.dll` and `nogi.dll`. GPU archives and DLLs have no fixed hash requirement. The renamed DXGI DLL bytes remain unchanged. No GPU implementation is replaced by this edit.

Python build dependencies are pinned in `requirements.txt`; their packages retain their own licenses. FFmpeg/FFprobe, Visual Studio, Python and Rustup are user-installed build tools. Microsoft SimSun and SimHei are read from the user's Windows installation and are not redistributed. The accepted title correction inputs contain a small reconstructed background region and alpha masks; original logo/image RGB is read from the user's retail disc during the build.

Original game assets and the game itself remain the property of their respective rights holders. External translations, including English patches, are separate user-provided inputs. No original disc content or external translation is licensed by this repository.
