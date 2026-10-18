# SMT NINE Recomp

《真·女神转生 NINE》原 Xbox 版的 Windows 原生静态重编译项目，基于 [xboxrecomp](https://github.com/sp00nznet/xboxrecomp)。使用者提供原始日版 ISO，在本地提取资源、生成游戏代码并编译程序。默认只构建日文，本项目中文翻译为可选项；英文通过 Config 导入使用者另行准备的已打补丁 ISO，不需要先构建中文。

仓库只提供源码、重建规则、中文翻译与稳定身份。源码仓库与源码 ZIP 不包含原游戏、生成的游戏 C、微软字体、存档或外部英文翻译；GitHub 发行另提供已有安装使用的纯程序更新包，不附日中原始数据。

## 运行配置与验证范围

| 项目 | 要求／已验证范围 |
| --- | --- |
| 操作系统 | Windows 10／11 x64。 |
| CPU | x86-64 处理器；最低 CPU 型号及游玩所需最低内存尚未实测确定。 |
| 显卡 | 运行时请求 Direct3D **功能级别 11_0**。D3D11 使用系统显卡驱动；D3D12 需要支持 D3D12 的驱动，通过微软 D3D11On12 工作；Vulkan／OpenGL 还须满足随附 DXVK／WineD3D 的驱动要求，仅支持某个 API 名称不代表画面一定正确。 |
| 图形内存 | **尚无经过验证的最低显存数字；512 MiB 配置尚未确认受支持。** 核显的专用显存不等于全部可用图形内存，还需看共享系统内存及分配给游戏进程的预算。 |
| 初次设置 | 建议先用 **1 倍、4:3、30 Hz**。4 倍的渲染目标像素数是 1 倍的十六倍，并不表示总显存占用也按相同比例变化。 |
| 本地构建 | 至少预留 **15 GB 磁盘空间**，建议 **16 GB RAM**；这是提取／编译建议，不是实测的游玩最低配置。 |

本机使用 AMD Ryzen 9 9950X 和 RTX 5090 对照了 D3D11／Vulkan／OpenGL／D3D12，另补测 AMD Radeon Graphics 核显的 D3D11。日文、1 倍、4:3、30 Hz 的街道采样约 29～31 FPS，未复现人物横向缺口。[进程显存采样数值](docs/VALIDATION.md#process-graphics-memory-samples)单独列出。这是测试参考配置，不是最低配置或全流程保证；玩家来件中的同一女性主角与餐厅场景仍待复现。

报告图形问题时，请提供发生异常那次运行的 `runtime/startup.log`、驱动版本和附近的普通存档。日志记录专用／共享容量；若所选运行库支持 DXGI 显存查询，还会在呈现画面时每五秒记录 local／nonlocal 内存段的进程用量 `current_bytes` 和预算 `budget_bytes`，不支持的查询会明确标注 unavailable。容量、进程用量和预算是不同数据，专用显存显示 512 MiB 不能单独证明显存耗尽。转换层可能返回兼容用的适配器名称与容量，请同时提供 Windows 显示的显卡／驱动信息。双击重新启动会覆盖日志，请在下次启动前复制。

Config 提供 Auto、Vulkan、OpenGL、D3D12 和 D3D11。Auto 在初始化失败时按 **Vulkan → OpenGL → D3D12 → D3D11** 尝试。D3D12 通过[微软 D3D11On12](https://learn.microsoft.com/en-us/windows/win32/direct3d12/direct3d-11-on-12)复用现有 D3D11 渲染代码，不保证更快或降低显存要求。

## 构建与启动

### 更新

- **已编译的游玩目录：** 关闭游戏和 Config，从[发行页](https://github.com/gymzatan/smt-nine-recomp/releases/latest)下载匹配日文／日中安装的**更新 ZIP**，解压到原游戏目录，运行 `Apply-Update.cmd`。
- **源码目录（含已导入英文）：** 运行 `git pull`，再用原构建环境执行 `.\update.cmd`，不需要 ISO。

保留游戏数据、设置和普通存档。[详细更新说明](docs/BUILDING.md#compiled-program-updates)。

### 首次安装

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

## 自定义控制

打开 **Config → 按键设置**，从 24 项 Xbox 游戏动作中选择一项，分别选择或捕获键盘按键与 XInput 手柄输入。包括双摇杆方向及按下、十字键、四个面键、Start／Back 和原 Xbox 白／黑／L／R 键。“不绑定”可禁用对应输入，“恢复默认”可恢复原键位。点击“应用”后回到主窗口“保存”，下次启动生效。设置保存在 `runtime/NINE.ini`，仅更新程序时会保留。

可交换或反转摇杆方向；映射到模拟动作时保留摇杆力度与扳机按压程度。实体输入仍只在游戏窗口位于前台时生效。F1～F5、F8、F9 保留即时档／截图／暂停功能，不能用作游戏动作键。“操作说明”与对应表展示默认键位。
