# 文件管理器（自研 Explorer 替代品）

用 C++17 + Win32 API + Shell COM 手写的 Windows 文件管理器，目标是日常替代系统自带的
文件资源管理器（`explorer.exe` 的**窗口**部分），同时保留 `explorer.exe` 负责桌面与任务栏。

> 说明：不替换 `explorer.exe` 进程本身。Windows 的桌面、任务栏、开始菜单都由
> `explorer.exe` 提供，强行替换属于不受支持的黑改方案，稳定性极差。
> 本项目的做法是：注册表接管「打开文件夹」动作，让双击文件夹时启动本程序。

## 技术选型

| 项目 | 选择 | 理由 |
|---|---|---|
| 语言 | C++17 | 操作 Shell COM 最顺手，生态案例最多 |
| 界面 | Win32 + ComCtl32 原生控件 | 交互贴近系统原生 |
| 目录枚举 | `IShellFolder` | 和资源管理器同底层，支持此电脑/回收站/网络等虚拟文件夹 |
| 文件列表 | ListView 虚拟模式（`LVS_OWNERDATA`） | 十万级文件不卡顿 |
| 文件操作 | `IFileOperation` | 复用系统原生复制/删除/回收站/撤销/冲突弹窗 |
| 编译器 | MSVC v143（VS2022） | 已在本机验证可用 |
| 第三方库 | 仅 header-only 小库（WRL、WIL、nlohmann/json、spdlog） | 运行时代价几乎为零，产物仍是单 exe |

## 目录结构

```
explorer/
├─ docs/                 架构与交接文档
├─ src/
│  ├─ core/              程序骨架、配置、异步任务
│  ├─ shell/             Shell COM 封装（IShellFolder / IContextMenu / IFileOperation）
│  ├─ ui/                界面层（主窗口、地址栏、文件列表、工具栏）
│  └─ main.cpp           程序入口
├─ res/                  资源文件（manifest、rc、图标）
├─ third/                第三方 header-only 库
├─ Explorer.sln          VS2022 解决方案
└─ Explorer.vcxproj      项目文件
```

## 编译

用 Visual Studio 2022 打开 `Explorer.sln`，选择 `Debug|x64` 或 `Release|x64`，直接 F5 / Ctrl+Shift+B。

命令行编译：

```powershell
& "C:\dev\vs2022\MSBuild\Current\Bin\MSBuild.exe" "C:\git\explorer\Explorer.sln" /p:Configuration=Debug /p:Platform=x64 /v:minimal
```

产物位置：`bin\x64\<Debug|Release>\FileExplorer.exe`

> 刻意命名为 `FileExplorer.exe` 而不是 `Explorer.exe`，避免和系统进程同名造成混淆。

## 当前进度

- [x] 阶段 0：骨架自检（主窗口 + COM 初始化 + ComCtl32 v6 + 消息循环）
- [ ] 阶段 1：Shell 层封装，`IShellFolder` 目录枚举
- [ ] 阶段 2：虚拟 ListView 文件列表
- [ ] 阶段 3：地址栏与导航历史
- [ ] 阶段 4：系统原生文件操作（复制/剪切/粘贴/删除/回收站）
- [ ] 阶段 5：原生右键菜单与 OLE 拖拽
- [ ] 阶段 6：配置持久化、图标缓存、性能开关
- [ ] 阶段 7：注册表接管、整体联调

## 编码约定

- 所有源码文件 UTF-8（无 BOM），编译加 `/utf-8`，保证中文注释正确解析
- 注释使用中文，标识符使用英文
- git 提交信息使用中文
