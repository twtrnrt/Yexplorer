# 多选文件右键菜单 WPS 展开慢 —— 排查记录（2026-11-26）

> 分支：`feature/context-menu`。本文记录从现象到定位的全过程、实测数据、已排除方向和当前结论；
> 临时探针埋点定位完要整体撤掉（清单见文末）。

## 1. 现象

- 多选大量文件时，右键菜单里 WPS 的「经典」(`IContextMenu`) 和「现代」(`IExplorerCommand`) 子菜单展开随文件数线性变慢；几百个文件要等几十秒。
- 用户纠偏：网络盘有影响但不可能这么大；现代比经典只慢一点也不符合实测；**只查本地盘、经典模式为什么文件越多越慢**。
- 测试目录：`C:\Users\kf168\Desktop\测试pdf`，282 个文件（222 个 .pdf，其余 .doc/.docx/.et/.icontest/.ppt/.pptx/.ttf/.wps/.xls/.xlsx 各 6 个，共约 833MB），本地盘。

## 2. 复现与测量方式

- 带临时探针的 Release 构建，产物 `bin\context-menu\Release\FileExplorer.exe`，日志同目录 `FileExplorer.log`（进程独占写，读取要用 `FileShare.ReadWrite` 流 + UTF-8）。
- 探针命令号 `kPerfProbe = 39001`（`WM_COMMAND` 0x0111）；全选命令号 3107；环境变量 `PERF_LIMIT` 截断只取前 N 个路径跑曲线。
- 流程：启动 exe 带目录参数 → 等「枚举完成」→ `PostMessage(3107)` 全选 → `PostMessage(39001)` 触发探针 → 等「探针结束」。
- 构建命令（功能分支显式隔离 OutDir/IntDir，禁用默认 `bin\x64`）：

```powershell
& "C:\dev\vs2022\MSBuild\Current\Bin\MSBuild.exe" Explorer.vcxproj /t:Build `
  /p:Configuration=Release /p:Platform=x64 `
  /p:OutDir="C:\git\explorer\bin\context-menu\Release\\" `
  /p:IntDir="C:\git\explorer\obj\context-menu\Release\\" /v:minimal /nologo
```

## 3. 第一轮分段计时（Release 探针，本地盘）

代码侧是按批传的，没有在文件循环里逐个调 handler：经典整批一次 `Initialize` + 一次 `QueryContextMenu`；现代整批一个 `IShellItemArray`，每命令一次 `GetTitle/GetState`。

### 经典 WPS（`kwpsshellext64.dll`，CLSID `{AF1D7D2F-6AE8-4BAA-ABFA-738201F4871C}`）

| N | CoCreate | Initialize | QueryContextMenu | 图标适配 | 总计 |
|---|---|---|---|---|---|
| 2 | 2 | 159 | 75 | 4 | 243 |
| 10 | 2 | 204 | 266 | 3 | 478 |
| 50 | 2 | 250 | 1132 | 3 | 1390 |
| 282 | 4 | 748 | 5908 | 4 | 6667 |

`QueryContextMenu` ≈ 21ms/文件。

### 现代 WPS（`kwpswinappext.dll`，3 个 owner 串行）

| N | 建数组(PIDL) | GetTitle | GetState | 子命令 | 单 owner 总计 |
|---|---|---|---|---|---|
| 2 | 5–7 | 104–110 | 0 | 43–46 | 395–424 |
| 50 | 88–109 | 1460–1524 | 0–1 | 51–61 | 1845–1933 |
| 282 | 555–596 | 7619–8019 | 0 | 57–63 | 8500–8914 |

`GetTitle` ≈ 27ms/文件；3 个 owner 串行，现代一行总约 25 秒。

### 对照组（N=282，证明不是我们传数据方式普遍出错）

7-Zip 经典 Query=0ms；TortoiseSVN 经典 Query=72ms；WinRAR 经典 Query=3ms；ima 经典 Query=1ms；WinRAR 现代 GetTitle=0ms；TortoiseSVN 现代 GetTitle=6ms/GetState=858ms；PDFToolExtension 现代 3 个 575–754ms 无产出。

## 4. 只读诊断：我们这批数据本身读起来贵不贵

在经典 `dataObject` 就绪后枚举格式、读 ShellIDList 项数、绑定父 `IShellFolder` 逐项读名/属性计时；现代数组逐项读 `IShellItem` 名/属性计时。

| 文件数 | 经典：父文件夹逐项读名+属性 | 现代：数组逐项读名+属性 |
|---|---|---|
| 50 | 0ms（50/50 取名成功） | 0ms |
| 282 | 6ms（282/282） | 1–2ms |

- dataObject 实际格式：`Shell IDList Array`、`#15(CF_HDROP)`、`FileName`、`FileNameW`、`FileContents`、`FileGroupDescriptorW`、`ZoneIdentifier`——与系统 `SHCreateDataObject` 标准产出一致，**不缺格式**。
- ShellIDList 项数与选中文件数一致（50、282）。

**排除**：缺剪贴板格式；PIDL 未绑定导致读属性贵；现代 IShellItem 从裸 PIDL 造出来读不动；我们传数据方式普遍出错。

## 5. 原版 Explorer 对照（方向 3）

用户在原版资源管理器里全选同样 282 个文件展开 WPS 菜单：**毫秒级，和选 2 个没区别**。

→ 排除「WPS 通病」。同一批文件、同一个 WPS DLL，原版喂进去秒开，我们喂进去逐文件慢 6～8 秒，**差异在我们传给 WPS 的上下文**。

## 6. 原生 PIDL / 数据对象数组对照（方向 2，探针内分支）

### 经典：换父文件夹 `EnumObjects` 原生枚举 child PIDL

绑定父 `IShellFolder` → `EnumObjects` 枚举原生 child PIDL，按文件名匹配选中项（282/282 命中），再 `SHCreateDataObject` 喂 WPS。

| 路径 | Initialize | QueryContextMenu |
|---|---|---|
| 正式（路径字符串 `SHParseDisplayName` 重建 PIDL） | 792ms | 6133ms |
| 对照（原生枚举 PIDL） | 26ms | 6108ms |

`QueryContextMenu` 几乎没掉 → **经典慢与 child PIDL 是重建还是枚举无关**。`Initialize` 792→26ms，那 700 多毫秒是逐文件 `SHParseDisplayName`/绑定成本，可优化但不是 6 秒大头；**6.1 秒的 `QueryContextMenu` 内部行为尚未定位**。

### 现代：从标准 dataObject 造数组（`SHCreateShellItemArrayFromDataObject`）

正式路径 `MakeShellItemArray` 用 `SHCreateShellItemArrayFromIDLists`（裸绝对 PIDL 列表造数组）；对照先 `BuildShellExtContext` 出标准 dataObject，再 `SHCreateShellItemArrayFromDataObject` 造数组。

| WPS 命令 CLSID | 正式 FromIDLists 标题 | 对照 FromDataObject 标题 |
|---|---|---|
| `{103CE0C1-0F7C-4B02-BEB5-2284CB3BEF7C}` | 8489ms | **2ms**，标题 `批量打印/PDF编辑/更多` |
| `{EA5C4860-EA21-45D6-836D-C891EF5F7D8C}` | 7471ms | **2ms**，标题正确 |
| `{EAE7D0A0-4863-4DA0-852C-342B00F68490}` | 8056ms | **2ms**，标题正确 |

对照数组项数=282、`titleHr=S_OK`、标题文本与正式一致，不是空标题假快。

## 7. 当前结论

### 现代（结论被二次缓存推翻，根因未重新定位）

- **二次缓存反转（2026-11-26 复测）**：同一 command、同一 `IShellItemArray`，连续调两次 `GetTitle`，第一次 8.4s、**第二次 0ms**（hr=S_OK）。说明 WPS 进程内对这批选中结果有缓存；此前对照函数跑出的 "2ms" 是**二次命中缓存**，并非 FromDataObject 更快。
- 因此旧结论"慢在 `FromIDLists` 数组 API、FromDataObject 仅 2ms"**不成立**：改成 FromDataObject 后首次 `GetTitle` 仍是 8.4s。数组来源差异被缓存混淆，未证实。
- 实测（282 项，FromDataObject 正式路径）：WPS 三个现代命令首次标题 8432 / 8462 / 8551ms，二次 0ms；非 WPS 现代命令（`{30351351-…}` 8ms、WinRAR 0ms）正常，慢是 WPS 特有。
- 待重新定位：首次 `GetTitle` 内部逐文件重活是什么；原版资源管理器同样 282 文件首次为何毫秒级，差异仍在传给 WPS 的上下文。dataObject 格式齐全、自读 0–1ms。

### 方案 A：逐项类型属性计时（2026-11-26，已排除属性读取慢路径）

现代数组逐项读三类属性，282 项累计：

| 属性 | 成功项 | 累计耗时 | 样例 |
|---|---|---|---|
| `System.FileExtension` 扩展名 | 282 | **0ms** | `.pdf` |
| `System.ItemTypeText` 类型名 | 282 | **≤3ms** | `WPS PDF 文档` |
| `System.ItemType` 关联类型(ProgID) | 282 | **0ms** | `.pdf` |
| `GetPropertyStore` 取属性存储 | — | 首次 955ms、之后约 150ms | — |

- 三类类型属性读取合计 ≤1 秒，**不是 8.3 秒 `GetTitle` 的大头**；扩展名/类型我们的 item 都能快速读到，排除"逐文件读 Shell 类型属性走慢路"。
- 8.3 秒在 `GetTitle` 内部的别的逐文件操作（候选：读文件头/内容/大小、QueryInterface 其他接口、内部枚举子命令），待方案 C（ProcMon）看那几秒实际碰了哪些文件/注册表/COM。

### 方案 C：ProcMon 抓现场（2026-11-26，过滤器只留 FileExplorer.exe，282 项）

> 抓包会给每次系统调用记账，耗时被放大约 10 倍；以下为抓包下的相对行为，绝对值不可直接对比。

**经典 QueryContextMenu（抓包 70.9s，19:15:27–19:16:34）**

- 每秒稳定 3300–4100 个事件，**铺满全程、无静默**；总计约 25 万次文件/注册表调用。
- 每个选中文件：`CreateFile`（Desired Access: **Read Attributes**, Open Reparse Point）+ `QueryBasicInformationFile` + `CloseFile` **各 322 次**（即同一文件被打开 322 轮）；不读文件内容。
- 注册表反复访问：`HKCU\Software\Kingsoft\Office\6.0\Common\kic\switch\user|remote`、`Classes\*\ShellEx\ContextMenuHandlers\Open With kwpsshellext`、相关 CLSID。
- 结论：经典慢是**逐文件重复打开读属性 322 轮**，属 shell 绑定/取属性路径问题；原版资源管理器不重复开文件。

**现代 GetTitle（3 条各抓包约 90s）**

- 事件密度呈"**前段爆发 + 长静默**"：前 3 秒约 3.6 万次系统调用，随后约 **65 秒只有 2 个事件**，收尾再一批。
- 前段批量：注册表 `Internet Explorer\Security`（DisableSecuritySettingsCheck）、`SyncRootManager`、`MountPoints2\CPC\Volume` 卷信息、目录查询、图标文件。
- `ReadFile` 集中在开头瞬时段，命中的是 6 个 `.pptx`（各约 893 次）、`.xlsx`/`.docx`，**222 个 PDF 没被读内容**。
- 65 秒静默期无文件/注册表/进程事件，ProcMon 文件注册表维度看不到；候选是跨进程 COM/RPC 阻塞、锁等待或 CPU 计算，需线程栈/ETW/CPU 采样才能进一步定位。

**工具产物**：`C:\dev\procmon\`，`filter.pmc`（FileExplorer 过滤器）、`trace2.pml`（3.75GB）、`trace2.csv`（211MB，过滤后）。

### 上网检索线索：查询阶段应截断项数（高置信度方向，2026-11-26）

微软官方两篇文档指向同一个机制——**菜单查询阶段本就不该把完整选择传给扩展**：

1. **多选谓词最佳实践**：「在 Windows 7 及更高版本中，查询快捷菜单时传递给谓词的项数限制为 16；调用该谓词时，谓词再依据完整选择重新创建并重新初始化」。谓词只应考虑第一项和总项数，「通常无需测试所选内容中的每个项目，也无需查询每个项的属性」。
   https://learn.microsoft.com/zh-cn/windows/win32/shell/verbs-best-practices
2. **谓词选择模型（MultiSelectModel）**：旧 IContextMenu 文档模型 15 项 / Player 100 项；COM 文档模型 15 / Player 无限制。`MultipleInvokePromptMinimum` 注册表默认 15，超过即不显示 Open/Print/Edit 类谓词。
   https://learn.microsoft.com/zh-cn/windows/win32/shell/context-menu-handlers
3. **`GetState` 的 `fOkToBeSlow`**：为 FALSE 时谓词不得做重内存/计算，应返回 `E_PENDING`；TRUE 才允许慢计算。
4. 现代 `IExplorerCommand` 官方示例：`GetTitle`/`GetState`/`GetIcon` 都不读 `items`，标题静态返回；只有 `Invoke` 才取完整 items。
   https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/integrate-packaged-app-with-file-explorer

**对照我们的实现**：菜单构建（查询）阶段就把完整 282 项塞进 `IDataObject` / `IShellItemArray` 传给 WPS。WPS 属「悲观、逐类型检查」型扩展，拿到多少项就逐项绑定/检查，于是随 N 线性变慢；7-Zip 等 Player/乐观谓词不逐项检查，所以正常。原版 explorer 查询阶段只传前 16 项（或第一项），完整选择留到 `InvokeCommand` 执行时重建——这层截断在系统 explorer 里是内置的，我们自己实现文件管理器没有这层，需要自己做。

**候选方案（待拍板，未动手）**：

- **方案 1（对齐官方，推荐）**：查询阶段（`Initialize`/`QueryContextMenu`/`GetTitle`/`GetState`）构造 dataObject、ShellItemArray 时只取前 16 项；点击执行（`InvokeCommand`/`Invoke`）阶段再用完整选择重建上下文。取舍：菜单可见性基于前 16 项类型，与原版一致；个别「前 16 项都不支持、后面才有支持项」的异构选择可能少显示一项，属官方接受的取舍。
- **方案 2（更激进）**：查询阶段只传第一项，执行阶段传完整。更快，但偏离 16 项惯例、异构判断更粗。
- **方案 3**：维持传完整选择，把扩展调用挪后台线程异步填菜单。改动大，且 STA/COM 线程亲和性问题多，不推荐。

**动手前待核查（已核对代码，2026-11-26）**：

- ① `GetState` 第二参 `fOkToBeSlow` 当前传的是 **`TRUE`**（`MenuInventory.cpp` 2589 行），等于允许现代命令在菜单构建阶段做重计算；按官方菜单阶段应传 `FALSE`（重活留到 `Invoke`，必要时返回 `E_PENDING`）。
- ② 执行阶段已按完整选择重建：`InvokePackagedCommand`（4388）重新 `CoCreateInstance` + 用 `targetPaths` 重新 `MakeShellItemArray`；`InvokeInventoryItem` 4560 行 `packagedPaths = targetPaths`（完整选择）。经典 `InvokeShellHandler` 同理。**执行链路无需改，只截断查询阶段即可。**
- ③ 16 这个上限是否要做成可配置，待拍板。

### 已落地的代码改动（Release 编译 0 错误，尚未实测验收）

1. **运行时展开按场景过滤**（独立有效，解决"3 套"）：`MainWndMenus.cpp` 遍历 `row.owners` 处，跳过 `owner.scenes != 0 && (owner.scenes & live.scene) == 0` 的 owner；`scenes==0` 放行兼容旧缓存。多选文件场景只留 `*` 场景的 `EAE7D0A0` 一套。
2. **数组改为 dataObject 来源**（方向更标准，但**已证实不是 8 秒主因**，去留待定）：`MakeShellItemArray` 增加 `parentFolder`，先 `BuildShellExtContext` 出 dataObject 再 `SHCreateShellItemArrayFromDataObject`；采集/执行/普查三处透传 `parentFolder`。
3. **查询阶段挑选代表项（2026-11-26 落地，Release 编译 0 错误，待复测）**：
   - 新增常量 `kQuerySelectionLimit = 16`（兜底上限，写死，未做配置项）。
   - 新增工具 `SelectQueryRepresentatives`：先按扩展名去重，每种扩展名保留原顺序第一个出现的文件；去重后仍超过 16 再截断到前 16 个代表。扩展名大小写不敏感，无扩展名归为同一类。
   - 统一采集入口 `CaptureOwnerMenuInternal` 用 `queryPaths` 挑代表项，经典 `CaptureHandlerWithMenu` 与现代 `CapturePackagedCommand` 都用它；执行阶段 `InvokeShellHandler` / `InvokePackagedCommand` 仍按完整 `targetPaths` 重建，不受影响。
   - `ExplorerCommandState` 调 `GetState` 的 `fOkToBeSlow` 由 `TRUE` 改 `FALSE`；返回失败（含 `E_PENDING`）时按默认启用处理。
   - 预期：WPS 这类悲观型扩展查询阶段只拿到「各扩展名代表 + 16 兜底」，绑定/检查量从 282 降到种类数；混合类型覆盖面比单纯前 16 更全。
   - **实测反馈（前 16 版本，2026-11-26）**：多选 282 文件右键 WPS 菜单展开速度明显变快，基本解决；去重版本待复测。

### 经典（未决）

- PIDL 来源已排除；dataObject 格式齐全、自读极快。`QueryContextMenu` 6.1 秒在 WPS DLL 内部。
- ProcMon 复核（见方案 C 段）：单文件 322 次打开**均匀铺满 67 秒**、每秒 3–8 次，属菜单构造过程中持续反复绑定/读属性，并非单轮遍历。`Initialize` 逐文件 `SHParseDisplayName`/绑定约 700ms，是次要可优化项。
- 未决：WPS 为何对同一选中集反复重新绑定；是否因第三参 `hkeyProgID=nullptr` 或 dataObject 缺少某信息触发其逐文件反查。需对比原版 dataObject/Initialize 参数，或逆向 `kwpsshellext64.dll`。

### 现代为什么有 3 套一样的 WPS 命令（已查明）

注册情况（注册表 `HKCR\PackagedCom\Package` + 各包 `AppxManifest.xml`）：

- WPS 装了 **3 个并列包**：`wpsappext1/2/3_1.0.0.2_neutral__km9wtz8rh8me0`，三者 manifest 几乎一样，只 Verb Id 后缀 1/2/3 不同，注册的是**同一批 4 个 CLSID**、同一个 DLL `kwpswinappext.dll`。代码枚举已按 `clsid|itemType` 去重，包层面的重复不会重复进菜单。
- 4 个 CLSID 对应 **4 种不同选中场景**（不是重复命令）：

| CLSID | manifest ItemType | 场景 | 登记 scenes |
|---|---|---|---|
| `{EAE7D0A0-4863-4DA0-852C-342B00F68490}` | `*` | 文件 | File \| Multiple |
| `{EA5C4860-EA21-45D6-836D-C891EF5F7D8C}` | `Directory` | 文件夹 | Folder |
| `{103CE0C1-0F7C-4B02-BEB5-2284CB3BEF7C}` | `Directory\Background` | 空白区 | Background |
| `{B5C96403-B820-4300-8A2F-CDFDBFCDFEF6}` | `Drive` | 盘符 | （代码落到默认 File 分支，未产出菜单） |

- **坑（导致多选时冒出 3 套）**：归组 `BuildAppSourceRows` 把同应用名、不同场景的 owner 都并到一行（`row.scenes` 取并集）；但运行时展开 `MainWndMenus.cpp` 遍历 `row.owners` 时**没有按 `owner.scenes` 过滤当前 `live.scene`**，于是多选文件场景把 Folder、Background 场景的 owner 也实例化了，各白跑一遍 `GetTitle`。设置窗口 / 菜单布局链路都有 `(scenes & sceneBit)==0` 过滤，唯独运行时展开这处漏了。
- 修复：遍历 owners 时跳过 `(owner.scenes & live.scene)==0` 的项。多选文件场景只该留 `*`（`EAE7D0A0`）那一套。场景位：File=1、Folder=2、Multiple=4、Background=8，`live.scene` 与 `owner.scenes` 同一套位。

## 8. 临时探针 / 埋点清单（定位完整体撤销，勿提交）

- `src/shell/MenuInventory.cpp`
  - 经典/现代各段 `[perf]` 分段计时日志；`DumpDataObjectDiagnostic`、`DumpItemArrayDiagnostic` 只读诊断；
  - 对照函数 `ProbeClassicNativePidl`、`ProbeModernNativeArray`（外部链接，供探针调）。
- `src/shell/MenuInventory.h`：两个对照函数声明。
- `src/ui/MainWndMenus.cpp`：`RunPerfProbe()` 探针实现 + `PERF_LIMIT` 截断 + WPS 对照调用。
- `src/ui/MainWnd.cpp`：命令分发挂 39001 探针分支。
- `src/ui/MainWnd.h`：`kPerfProbe=39001` 常量 + `RunPerfProbe()` 声明。
- 注意：`MainWndMenus.cpp` 里有一处他人并行改动（`SetMenuItemInfoW(menu, position, FALSE, &mii)` → `TRUE`），撤埋点时不可覆盖。

## 9. 环境与备份

- 配置：`%APPDATA%\FileExplorer\settings.json`、`context-menu.json`、`menu-owners.json`；测试前备份在 `%TEMP%\explorer-perf-bak\`（settings.json、context-menu.json），测完还原 settings.json。
- WPS 经典 CLSID/server：`C:\Users\kf168\AppData\Local\Kingsoft\WPS Office\12.1.0.29690\office6\kwpsshellext64.dll`，InprocServer32 / Apartment。
