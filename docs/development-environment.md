# 本机开发环境

本轮只准备原生插件开发环境；曲线、快捷键和 Timeline 交互需求另行处理。

## 已确认的组件（2026-09-12）

| 组件 | 版本或位置 |
| --- | --- |
| 项目 | `C:\Users\dao\Desktop\codex\dalimao\AE_plugin_Dalimao_Curve` |
| After Effects | AE 2025，`AfterFX.exe` 文件版本 25.4 |
| AE Support Files | `C:\Program Files\Adobe\Adobe After Effects 2025\Support Files` |
| C++ 构建工具 | VS 2022 Build Tools，MSBuild 17.14.40，v143 / MSVC 14.44.35207 |
| MSBuild | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe` |
| Windows SDK | 10.0.26100.0 |
| Python | 3.14，`C:\Python314\python.exe`，通过 `py` 启动 |
| AE SDK | 25.6 build 61，见下面的相对路径 |

本机也有 VS 2026 Build Tools 和 AE 2020。本项目本次使用 VS 2022/v143 与 AE 2025。

## SDK 解压与引用

SDK 根目录：

```text
AfterEffectsSDK_25.6_61_win/ae25.6_61.64bit.AfterEffectsSDK/
```

用户提供的外层 ZIP 内含 Zstandard 压缩的内层 ZIP。已用 Python 3.14 的 ZIP/Zstandard 支持解压两层，并通过内层完整 CRC 校验。也可按压缩包自带 `README-HowToExtractZstdBuild-Win.txt` 的说明使用随附解压工具。SDK 是供 C++ 编译引用的头文件、工具和示例，无需复制到 AE 的插件目录。

已确认存在：

- `Examples/Headers/AE_GeneralPlug.h`、`AEConfig.h`
- `Examples/Headers/SP/SPBasic.h`
- `Examples/Util/AEGP_SuiteHandler.h`
- `Examples/Resources/PiPLtool.exe`
- `Examples/AEGP/Panelator/Win/Panelator.vcxproj`
- `After_Effects_SDK_Guide.pdf`（本轮未逐页审阅）

SDK 压缩包和解压目录均由 `.gitignore` 排除，不纳入源码提交。

## 开发流程

1. 阅读 `README.md`、`AGENTS.md` 和与当前需求有关的 `docs/` 文档。
2. 确认 SDK 引用，使用 Release/x64、v143 和 C++20 编译。
3. PiPL 资源向 AE 声明插件身份和入口；修改插件身份时必须同步资源。本项目已有资源文件，普通曲线修改无需重新设计 PiPL。
4. 构建成功后关闭 AE，再将 DLL 以 `DalimaoCurves.aex` 安装到 `Plug-ins/DalimaoCurves/`。
5. 启动 AE，检查 Window 菜单中的 DalimaoCurves，再测试自定义快捷键、面板和关键帧操作。
6. 功能修改需使用 `test project.aep` 做手工回归，检查撤销和退出行为，以及插件目录中的 `debug/DalimaoCurves.log`。旧版本工程的转换副本不得覆盖原始测试工程。

原生插件入口已通过 [Adobe After Effects 开发者网站](https://developer.adobe.com/after-effects/) 核对。具体 v143 配置、PiPL 构建步骤和 AEGP 示例以本地 SDK 的 Panelator 工程为参考。

## 范围与兼容性

SDK 25.6 与已安装宿主 25.4 版本不同。构建成功不等于已验证所有运行时 suite 或曲线行为；需要在 AE 25.4 中实际加载并回归。上一轮 Timeline 悬停/Graph Editor 工作流仍见 `timeline-hover-workflow.md`，本轮不实现该需求。

## 常用命令与路径覆盖

在项目根目录的 PowerShell 中：

```powershell
py deploy.py --check-env
py deploy.py --build-only
# 安装步骤需要关闭 AE，并在管理员 PowerShell 中执行：
py deploy.py --build
py deploy.py --verify
```

`py deploy.py` 只安装现有构建。`--verify` 核对文件存在；本地 DLL 存在时还会比对 SHA-256，不一致时返回非零退出码。它不验证 AE 运行时行为。

构建与安装路径由 `deploy.py` 集中配置，支持环境变量 `MSBUILD_PATH`、`AE_SDK_ROOT`、`AE_SUPPORT_DIR`、`AE_VERSION`。更换 AE 时，后两项需一起设置；`AE_VERSION` 用于可选插件缓存清理，当前默认 25.4。SDK 根目录指向包含 `Examples` 的目录。直接调用 MSBuild 时可传 `/p:AESDKRoot=...`，默认引用项目内的 SDK。

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe' `
    .\DalimaoCurves\DalimaoCurves.vcxproj `
    /p:Configuration=Release /p:Platform=x64 /t:Rebuild
```

本次遇到宿主进程环境同时包含 `Path` 和 `PATH`，MSBuild 在启动编译器时报告 MSB6001。`deploy.py` 在启动 MSBuild 前去重环境变量，建议优先使用上述 Python 构建命令。构建完整输出保存在 `x64/build.log`。

常规安装不清理缓存。只有排查插件扫描问题时才使用 `py deploy.py --build --clear-cache`。`deploy_restart.ps1` 是可选助手，会尝试保存并关闭已打开的测试工程，然后编译安装并重新打开；本次没有执行该助手。

`py make_pipl.py DalimaoCurves` 始终输出到 `DalimaoCurves/pipl_data.bin`，与 RC 引用保持一致；本次没有重新生成或更改插件身份资源。

## 本次验证

- SDK 两层解压完成，内层 ZIP CRC 检查通过，所需 Headers/SP/Util/PiPL 工具均存在。
- `py deploy.py --check-env` 通过。
- `py deploy.py --build-only` 通过，Release/x64、v143，0 个警告、0 个错误。
- 生成 DLL 为 136,704 字节；`dumpbin` 确认 x64 DLL、资源表和导出 `EntryPointFunc`。
- 已通过 Windows 管理员提权安装到 AE 2025 的 `Plug-ins/DalimaoCurves/DalimaoCurves.aex`，安装文件与构建 DLL 的 SHA-256 一致：`1a178ca71ab89b7002dfa5f82345b68950423ff23ee9ab3000cee7012ee8c4ed`。
- 构建前已有的 `DalimaoCurves.cpp` 校准边界修复予以保留，本轮未修改曲线逻辑。
- 原始文件备份保存在已忽略的 `x64/environment-backup/`，没有提交或推送。
- 未验证：AE 内加载、快捷键、曲线交互、撤销和测试工程回归。
