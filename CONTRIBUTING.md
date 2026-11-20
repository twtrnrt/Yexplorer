# 开发与发布规则

> 这份文件是本仓库的**硬规则**，所有参与的人（以及接手的 AI 会话）都按它做。
> `docs/交接-2026-11-19.md` 是某一轮的状态快照；两者冲突时，**以本文件为准**。

---

## 1. 发布铁律：一次做完

每发一版，按下面的顺序**一次做完**，不要拆成几次：

```
改版本号 → 全量构建 → 打包 → 启动冒烟 → 提交 → 打 tag → push
```

做完这七步，下面五样东西必须互相对齐：

- `res/version.h` 和 exe 的版本信息都是 `X.Y.Z`；
- `dist\X.Y.Z\` 里的 exe 就是这次发布提交构建出来的；
- 发布提交在 `master` 上；
- tag `vX.Y.Z` 指向这次发布提交；
- `origin/master` 和 `vX.Y.Z` 都推送成功。

> 2026-11 的教训：1.6.0 只推了远端 tag、没有和本地对齐；1.6.1 / 1.6.2 打了版本号却没打
> tag；1.6.3 第一版包打好后代码又改了。这些都是因为“发布”被拆成好几次做。

---

## 2. 发布步骤（照抄命令）

以下 `X.Y.Z` 换成实际版本号；命令都在仓库根目录执行。

### 2.1 改版本号

**只改 `res/version.h` 一处**：

- `YEXP_VER_MAJOR / MINOR / PATCH / BUILD` 四个数字；
- `YEXP_VER_STRING` 手写同步（`rc.exe` 不会跟着宏变，必须两处一致）。

README 顶部的「当前版本 **X.Y.Z**」也一起改。

### 2.2 全量构建与自检

```powershell
& 'C:\dev\vs2022\MSBuild\Current\Bin\MSBuild.exe' Explorer.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
& 'C:\dev\vs2022\MSBuild\Current\Bin\MSBuild.exe' Explorer.vcxproj /t:Rebuild /p:Configuration=Debug   /p:Platform=x64 /v:minimal /nologo
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\check-version.ps1 -RequireAll
```

要求：Release / Debug 都是 **0 错误 0 警告**；`check-version -RequireAll` 里
Release、Debug、`dist\X.Y.Z\` 三个 exe 全是 `X.Y.Z.0`。

### 2.3 打包

`dist\X.Y.Z\` 只放 **4 个文件**：

| 文件 | 来源 |
|---|---|
| `FileExplorer.exe` | `bin\x64\Release\FileExplorer.exe` |
| `LICENSE` | 仓库根目录 |
| `README.md` | 仓库根目录（含最新版本号） |
| `使用说明.txt` | 从上一版复制后改：第一行版本号 + 新增一段「【X.Y.Z 这次改了什么】」 |

要求：

- **不带 `FileExplorer.log`**、pdb、obj 等任何运行/中间产物；
- `使用说明.txt` 只写用户能感知的变化；内部重构要注明「行为不变」；
- 压缩成 `dist\Yexplorer-vX.Y.Z-win64.zip`，zip 里就是这 4 个文件，**不要多一层目录**：

```powershell
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    "dist\X.Y.Z", "dist\Yexplorer-vX.Y.Z-win64.zip",
    [System.IO.Compression.CompressionLevel]::Optimal, $false)
```

### 2.4 启动冒烟

直接运行 **包里的** exe（不是 `bin\` 里的），指向一个临时目录即可，检查日志：

```powershell
.\dist\X.Y.Z\FileExplorer.exe "C:\Users\<你>\AppData\Local\Temp\release-smoke"
```

日志里必须有「配置已加载」「系统图标列表初始化成功」，**0 个 `[error]`**。
跑完**删掉 `dist\X.Y.Z\FileExplorer.log`**，再打 zip（顺序不能反）。

### 2.5 提交

```powershell
git add res/version.h README.md docs/...
git commit -m "发布 X.Y.Z：版本号 + 发布包（含 …）"
```

- `dist/` 在 `.gitignore` 里，发布包不进库；
- 提交信息格式固定为 `发布 X.Y.Z：版本号 + 发布包（含 …）`，和 1.6.1 / 1.6.2 / 1.6.3 一致。

### 2.6 打 tag

用**附注 tag**（不是轻量 tag），指向发布提交（通常就是 HEAD）：

```powershell
git tag -a vX.Y.Z -m "Yexplorer X.Y.Z：一句话说明这版的主要变化"
```

### 2.7 push

```powershell
git push origin master
git push origin vX.Y.Z
```

**禁止 `git push --tags`**：本仓库的 `v1.6.0` 本地和远端指向不同提交，`--tags`
会撞上这个历史 tag。以后每个 tag 都**显式推**。

### 2.8 推完检查

```powershell
git status --short                                  # 应为空
git rev-list --count origin/master..master          # 应为 0
git ls-remote --tags origin "refs/tags/vX.Y.Z*"     # 能看到 tag 对象和 ^{} 提交
```

GitHub 上 tag 指向的提交必须等于发布提交。

---

## 3. 发布后又改了代码怎么办

- **已经打 tag / 已经 push 的版本不要再改它**；新改动属于下一个版本，升版本号重走一遍流程。
- 如果 tag 还没推、包还没发出去：可以重新打包，并把 tag 重新打在同一个发布提交上。
- 任何时候都**不要 force push 已推送的 tag**；`v1.6.0` 的历史不一致要修，先问用户。

---

## 4. 日常改动规则

- **每次提交都必须自己能编译、能启动**；提交前 Release + Debug 均为 0 错误 0 警告。
- **提交粒度**：一个主题一个提交；提交信息中文；不 `--no-verify`、不 `git push -f`。
- **源码编码**：UTF-8 无 BOM，编译带 `/utf-8`；注释中文、标识符英文。
- **新文件**要加进 `Explorer.vcxproj`（`ClCompile` / `ClInclude`）。
- **改 UI / 规则引擎要跑回归**：

  ```powershell
  powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\run-tests.ps1
  powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\e2e-batch-rename.ps1
  powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\e2e-mainwindow.ps1
  ```

- **文档**：每完成一块更新 `docs/进度与状态.md`（最新在最上面）；
  踩到新坑写进 `docs/踩坑记录.md`。
- **用户数据**：`%APPDATA%\FileExplorer\settings.json` 是真实配置，
  测试前备份、测完还原；`settings.json.bad` 不要删。

---

## 5. 和用户约定的发布纪律

- push 和 tag 属于**发布动作**。用户说“先别发”时，只提交、不推送。
- 用户确认发版时，按第 1 节**一次做完**，不要漏 tag、不要漏 push。
- 发布包、tag 的说明要用中文；`使用说明.txt` 面向最终用户，不要写内部代号。
