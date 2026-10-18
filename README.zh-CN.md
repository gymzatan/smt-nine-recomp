# SMT NINE Recomp

《真·女神转生 NINE》原 Xbox 版的 Windows 原生静态重编译项目，基于 [xboxrecomp](https://github.com/sp00nznet/xboxrecomp)。使用者提供原始日版 ISO，在本地提取资源、生成游戏代码并编译程序。默认只构建日文，本项目中文翻译为可选项；英文通过 Config 导入使用者另行准备的已打补丁 ISO，不需要先构建中文。

仓库只提供源码、重建规则、中文翻译与稳定身份。不包含原游戏、生成的游戏 C、微软字体、存档或外部英文翻译。

先安装 Windows x64、Python 3.14、Visual Studio 2022 C++ Build Tools（Windows SDK、CMake、Ninja）和 Rustup。只有选择中文构建时，才需要 PATH 中的 FFmpeg／FFprobe，以及 Windows 简体中文补充字体中的宋体和黑体。

构建后两个 exe 均位于根目录：直接双击 `smt9_recomp.exe` 游玩，双击 `Config.exe` 设置，不需要启动 cmd。程序按 exe 所在目录定位资源，可将整个目录移动到其他盘符。默认构建采用英文设置界面和日文游戏；启用中文构建时，两者默认中文。已有设置不会被覆盖。

```bat
setup.cmd
build.cmd --iso "D:\Games\Shin Megami Tensei - Nine (Japan).iso"
Config.exe
smt9_recomp.exe
```

若需要中文，在首次构建时显式添加选项：

```bat
build.cmd --iso "D:\Games\Shin Megami Tensei - Nine (Japan).iso" --with-chinese
```

默认构建从原盘生成程序和宽屏标题资源，跳过中文资源复制、文本、字库和结局字幕生成。`--with-chinese` 才执行这些中文资源步骤。构建保留原音轨，不调用翻译或图像生成服务。支持的 ISO 身份、工具设置和中断恢复见 [构建说明](docs/BUILDING.md)，实际构建及运行检查见 [验证范围](docs/VALIDATION.md)。

Config 的界面语言与游戏语言独立，三种界面语言始终可用。游戏语言列表默认只有日文，启用中文构建才显示中文。只玩日文和英文时，使用默认构建后，选择“导入或更新语言 ISO…”（英文界面为 **Import or update language ISO…**），语言标识与显示名称填写 `English`，再选择已经打好英文补丁的 ISO；导入完成后选择新增语言并保存。本项目不分发英文翻译补丁。

完整的手柄／键盘逐键对应表、窗口操作和即时存档快捷键见[中文操作说明](docs/CONTROLS.zh-CN.md)，也可在 Config 中点击“操作说明”。

当前已验证的流程与尚未覆盖的边界见 [状态说明](docs/STATUS.md)。尚未完成全流程通关和所有场景稳定 60 FPS 验收。

项目自有代码与中文翻译采用 MIT 许可；第三方组件保留各自许可，见 [第三方声明](THIRD_PARTY.md)。宋体、黑体只从使用者的 Windows 安装中读取，不随仓库分发。
