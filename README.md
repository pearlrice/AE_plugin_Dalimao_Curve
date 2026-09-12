# Dalimao Curves

AE AEGP 插件：快捷键切换 AE 自带的图形编辑器，并显示曲线预设按钮、手柄长度滑块和两帧曲线模板工具。

## 使用

1. 在 AE「编辑 > 键盘快捷键」中给 Window 菜单命令 **DalimaoCurves** 绑定快捷键，例如 `4`。保留 AE 自带的 `Shift+F3` 为图形编辑器切换键。
2. 选中一个带关键帧的图层，按下快捷键。时间轴切换为 AE 原生图形编辑器，同时显示工具窗口；已经显示曲线时不会反向切换。
3. 选择属性、相邻关键帧段落和维度。六个按钮提供线性、标准缓动、出长入短、出短入长、双侧较长和双侧最长。
4. 用两个滑块调整起点的出手柄和终点的入手柄长度（0.1%–100%）。松开时应用一次 AE 撤销组，已有贝塞尔速度保持不变；曲线形状、关键帧位置和值可以直接在 AE 原生编辑器中调整。
5. 可保存当前两帧曲线为模板，之后在其他段落复用；支持应用、删除、从磁盘重载。
6. 再次按下同一个快捷键，或点击 **返回图层滑条**，工具窗口关闭，时间轴返回图层滑条。焦点在工具窗口时也可按 Esc。

工具窗口不会因鼠标移出而关闭。AE 在前台时，工具窗口每 0.5 秒读取当前目标属性的变化；更换图层后点击 **读取 AE 选择**。如果关键帧在操作排队期间被更改，本次操作会停止并刷新，避免误写。详见 [曲线预设与模板](docs/curve-presets.md) 和 [原生曲线切换说明](docs/native-graph-editor.md)。

## 构建与部署

```powershell
py deploy.py --check-env   # 检查本机依赖路径
py deploy.py --build-only  # 仅编译 Release/x64，不安装、不操作 AE
py deploy.py --build       # 编译并安装；先关闭 AE，安装需要管理员权限
py deploy.py --verify      # 检查安装文件，并与当前构建进行 SHA-256 比对
```

本机目标为 AE 2025（25.4）的 `Support Files\Plug-ins\DalimaoCurves\`。构建产物为 `DalimaoCurves\x64\Release\DalimaoCurves.dll`，安装时命名为 `DalimaoCurves.aex`，重启 AE 后加载。检测到 AE 正在运行时，安装会停止；覆盖已有插件前会备份。

SDK 已解压到项目内，工程使用相对路径引用。完整的组件版本、配置覆盖方法、开发流程和验证结果见 [开发环境说明](docs/development-environment.md)。

## 结构

- `DalimaoCurves\` 插件源码工程（VS2022 / v143 / x64 Release）
- `deploy.py` 环境检查、编译、安装和文件验证；构建日志在 `x64/build.log`
- `deploy_restart.ps1` 可选的测试工程关闭/保存、编译安装和重新打开助手；使用前先阅读脚本
- `make_pipl.py` PiPL 生成脚本（改插件名时使用）

## 说明

- 独立 git 仓库，改动在本地提交，不推送（除非明确要求）。
- 原生面板诊断日志写入 `%APPDATA%\DalimaoCurves\native-graph.log`。工具窗口标题为 `Dalimao Curves 2 · 原生曲线工具`，用于区分仍带自绘曲线的旧版本。
