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
- 测试会读写 `%APPDATA%\FileExplorer\settings.json`：先备份，测完还原。
