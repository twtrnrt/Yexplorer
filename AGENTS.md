# AGENTS.md

本仓库的开发、构建、测试和**发布流程**以 [`CONTRIBUTING.md`](CONTRIBUTING.md) 为准。

接手时按这个顺序读：

1. [`docs/交接-2026-11-19.md`](docs/交接-2026-11-19.md)：当前状态、未完成项、构建命令、工具陷阱；
2. [`docs/进度与状态.md`](docs/进度与状态.md) 最上面几节：最新进展；
3. [`CONTRIBUTING.md`](CONTRIBUTING.md)：开发与发布硬规则。

必须记住的几条：

- 发布**一次做完**：改版本号 → 全量构建 → 打包 → 冒烟 → 提交 → 打附注 tag → push；
- **不要 `git push --tags`**（`v1.6.0` 本地/远端不一致），tag 要显式推 `git push origin vX.Y.Z`；
- 不要 `git push -f`、不要 `--no-verify`；
- **提交前必须先确认当前分支**（`git branch --show-current`）：本仓库存在并行工作（别人会在同一工作区
  `git checkout` 到 `feature/*` 分支改代码），当前分支可能在你不知情时就变了，`git commit` 会跟着当前分支走。
  2026-11-23 就因此把一条文档提交落到了 `feature/menu-config` 上（事后用 revert 收拾）。提交前还要看一眼
  `git status --short`：**工作区出现不是自己改的 `M` 文件，说明有人在里面干活，此时不要切分支、不要 reset**；
- 测试会读写 `%APPDATA%\FileExplorer\settings.json`：先备份，测完还原。
- **构建目录按分支隔离**：`bin\x64\Release`、`bin\x64\Debug` 只给 master 发布/主分支构建；功能分支必须显式传
  `/p:OutDir=<仓库>\bin\<分支名>\Release\`、`/p:IntDir=<仓库>\obj\<分支名>\Release\`（Debug 同理），
  绝不沿用默认 `bin\x64`。
