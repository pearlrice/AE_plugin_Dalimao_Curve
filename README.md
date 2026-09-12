# Dalimao Curves

AE AEGP 插件：通过快捷键呼出**关键帧贝塞尔曲线面板**，在面板中直接调整关键帧的缓动曲线。

## 功能

- 在 AE「编辑 > 键盘快捷键」中给 Window 菜单命令 **DalimaoCurves** 绑定一个快捷键。
- 选中一个图层（属性上带关键帧），按下快捷键后弹出曲线面板（与 DalimaoShortcuts 相同的呼出方式：Win32 分层窗口 + GDI+ 绘制）。
- 面板顶部下拉框列出当前图层所有带关键帧的属性，多维属性（位置/缩放等）可切换 X/Y/Z 维度。
- 图形区按 AE 实际求值采样绘制曲线，关键帧为圆点：
  - 拖动关键帧圆点 = 修改该维度的值；
  - 选中关键帧后显示左右贝塞尔手柄，水平拖动 = 修改影响（influence），垂直拖动 = 修改速度（speed）；
  - 修改实时写回 AE，支持 Ctrl+Z 撤销。
- 关闭方式与 DalimaoShortcuts 的搜索菜单一致：鼠标移出面板外一定距离自动关闭，Esc / 右键也可关闭。

## 构建与部署

```cmd
python deploy.py --build --clear-cache
```

输出到 AE 2024 `Plug-ins\DalimaoCurves\`，部署后重启 AE 生效。

## 结构

- `DalimaoCurves\` 插件源码工程（VS2022 / v143 / x64 Release）
- `deploy.py` 编译 + 部署脚本（路径按本机配置）
- `make_pipl.py` PiPL 生成脚本（改插件名时使用）

## 说明

- 独立 git 仓库，改动在本地提交，不推送（除非明确要求）。
- 调试日志写入插件目录 `debug\DalimaoCurves.log`。
