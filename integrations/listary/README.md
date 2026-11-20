# Listary 插件（Yexplorer）

让 Listary 的「快速切换（Quick Switch）」和「在文件管理器中打开」支持 Yexplorer。

## 它解决什么问题

Listary 对资源管理器之外的第三方文件管理器是**插件式**的：它从
`Listary\FileAppPlugins\<名字>\` 加载插件 DLL，对每个顶层窗口调用 `BindFileWindow`；
插件认领自己的窗口后，通过 `GetCurrentFolder` 报告当前目录。Listary 拿到目录后
负责把保存/打开对话框跳过去。

没有这个插件时，Listary 看不到 Yexplorer，快速切换自然不生效——这跟
`IShellWindows` 注册是两条独立的路（后者只让 Shell 认识我们）。

## 目录结构

```
integrations/listary/
  YexplorerPlugin.cs            插件源码（C# 5，.NET Framework 4.8）
  ListaryPluginManifest.json    给 Listary 的清单
```

## 构建 + 安装

```powershell
# 只构建到 dist\listary-plugin\
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-listary-plugin.ps1

# 构建并安装到 Listary（需要一次 UAC）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-listary-plugin.ps1 -Install
```

安装位置：`C:\Program Files\Listary\FileAppPlugins\Yexplorer\`。

卸载：删掉上面这个目录，重启 Listary 即可（不写注册表、不改 Listary 设置）。

## 验证

1. 打开 Yexplorer 到任意目录；
2. 打开一个保存/打开对话框，按 `Ctrl+G`（或先点 Yexplorer 再切回对话框）；
3. 对话框应跳到 Yexplorer 当前目录。

排查：设置环境变量 `YEXP_LISTARY_DEBUG=1` 后重启 Listary，
插件会把 `Initialize` / `BindFileWindow` / `GetCurrentFolder` 写到
`%TEMP%\yexp-listary-plugin.log`。

## 已知限制

- 目录来自窗口标题（`文件管理器 - C:\path（N 项）`），所以：
  - 桌面/文档/图片/音乐/视频/下载会映射到真实路径；
  - 「此电脑」「网络」、压缩包内部等没有真实路径的位置返回空（Listary 会跳过）。
- 只匹配类名 `FileExplorerMainWindow` 且进程名 `FileExplorer.exe` 的窗口。
- 插件依赖 Listary 的 `Listary.FileAppPlugin.dll` 和
  `Microsoft.Extensions.Logging.Abstractions.dll`（构建脚本会从 Listary 安装目录复制）。
- Listary 升级后如果插件接口变了，需要按新接口重新编译。

## 分发

这是**独立集成**，不打包进 Yexplorer.exe。发布时可以把
`dist\listary-plugin\` 一起放进压缩包，并在使用说明里写：
「装了 Listary 的话，运行 `tools\build-listary-plugin.ps1 -Install`（或手动复制
`dist\listary-plugin` 到 `Listary\FileAppPlugins\Yexplorer`）」。
后续如果做进设置界面，可以加一个「安装 Listary 支持」按钮（需要 UAC）。
