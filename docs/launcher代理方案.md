# explorer.exe 薄代理（让第三方程序的“打开所在文件夹”走本程序）

## 为什么需要它

很多程序（微信、QQ、批量打印软件等）的“打开所在文件夹”是这么写的：

```csharp
Process.Start("explorer.exe", "/select,\"" + path + "\"");
```

它**直接按名字启动 explorer.exe**，根本不问 shell“文件夹该用什么打开”。
所以「注册为默认文件管理器」写的那两个键
（`HKCU\Software\Classes\Directory\shell\open\command`、`Drive\shell\open\command`）
完全管不到它——这是程序自己的行为，不是注册没做好。

Windows 按名字找 exe 的顺序是：

1. 发起程序自己的目录
2. 当前工作目录
3. **System32**
4. Windows 目录（`C:\Windows\explorer.exe` 在这里）
5. …最后才是 PATH

`System32` 排在 Windows 目录**前面**，所以往 `System32` 放一个同名的
`explorer.exe`，所有“按名字启动 explorer.exe”的调用都会先命中它。
改 PATH 没用（第 4 项先命中），IFEO 也不合适（同名映像一律再劫持，无法把请求交回真 explorer）。

## 它是什么

一个很小的 exe（Release 静态链接，只依赖 `SHELL32/ADVAPI32/KERNEL32`，不需要 VC 运行库），
编译产物名字就叫 `explorer.exe`。它**不做文件管理**，只做参数分流：

| 收到的参数 | 交给谁 |
|---|---|
| `/select,"C:\a\b.txt"` | 本文件管理器（打开所在目录并选中该文件） |
| 盘符路径 `C:\dir` / UNC `\\server\share` | 本文件管理器（打开该目录） |
| `/e`、`/n`、`/root`、`/separate`、`/idlist`、`shell:xxx`、`::{CLSID}`、无参数 | 原样交给 `C:\Windows\explorer.exe` |

看不懂的全部直通系统——**这是安全阀，保证不会因为我们而出现“打不开东西”**。

### 它怎么找到本文件管理器

读「注册为默认文件管理器」时写下的那条命令：

```
HKCU\Software\Classes\Directory\shell\open\command
值形如： "D:\...\FileExplorer.exe" "%1"
```

取引号里的 exe 全路径。以下情况一律直通系统：

- 这个键不存在（没注册过默认管理器）；
- 取到的文件名就是 `explorer.exe`（用户已经恢复系统默认，防递归）；
- 取到的路径文件不存在（程序被删/被挪）。

### 为什么不会递归

直通系统时用**完整路径** `C:\Windows\explorer.exe`，不会命中 System32 里的同名文件。

## 放置方法

### 自动部署（推荐）

点设置里的「注册为默认文件管理器」时，程序会自动把代理放进
`C:\Windows\System32\explorer.exe`：

| System32 里当前的状态 | 程序的处理 |
|---|---|
| 没有这个文件 | 提权部署（弹一次 UAC） |
| 有，且是我们放的、内容还是最新的 | 什么都不做，不弹 UAC |
| 有，是我们放的旧版本 | 提权替换（弹一次 UAC） |
| 有，**但不是我们放的**（系统文件/别的软件/手工放的旧版） | 只弹窗提醒，绝不覆盖 |

点「恢复系统默认文件管理器」时：如果那个文件是我们放的，会提权删掉；不是我放的就不动。

**怎么认出“是我们放的”**：代理 exe 里嵌了版本资源（`src/launcher/res/launcher.rc`），

```
ProductName      = "Yexplorer Launcher"
OriginalFilename = "explorer.exe"
```

这两个值是固定标记，写死在文件里，不依赖注册表，重装、换目录都认得出来。
判断逻辑见 `src/shell/ExplorerProxy.cpp` 的 `IsOursProxy`。

**代理从哪来：已经焊在主程序里，发布包不带额外文件**

代理的二进制作为 RCDATA 资源嵌进了 `FileExplorer.exe`
（`res\app.rc` 的 `IDR_EXPLORER_PROXY`，ID 定义在 `res\resource.h`，
payload 文件是 `res\explorer-proxy-x64.exe`）。
部署时程序自己释放：`FindResource` → 写 `%TEMP%\Yexp\proxy\explorer-proxy-x64.exe`
→ `CopyFile` 到 System32 → 删掉临时文件。

所以：

- 发布压缩包**只有一个 exe**，不需要（也不应该）再附带 `explorer-proxy.exe`；
- 主程序体积 +176 KB，这是把代理焊进去的代价（文件总数少了）；
- 改过代理源码后，要先跑 `tools\update-proxy-payload.ps1` 刷新 payload，
  否则焊进去的还是旧代理（脚本会打印当前 payload 的版本标记供核对）。

万一某个构建没有这个资源，程序会退回老做法：找同目录的 `explorer-proxy.exe`，
找到就部署，找不到才提示失败。

**第一次的特殊情况**：如果以前手工往 System32 复制过一份**没有版本标记**的旧代理，
程序认不出它，会弹「不是本程序放的」提醒。这时手动删掉
`C:\Windows\System32\explorer.exe`，再点一次注册即可。

### 手工放置（备用）

编译产物（Release）：

```
bin\launcher\x64\explorer.exe   →  复制到  C:\Windows\System32\explorer.exe
bin\launcher\x86\explorer.exe   →  复制到  C:\Windows\SysWOW64\explorer.exe
```

- 64 位进程发起调用走 System32，**32 位进程走 SysWOW64**。
  SysWOW64 那份默认不做：那里的 `explorer.exe` 是系统原有文件（所有者 TrustedInstaller），
  替换要先 `takeown` 夺权、`sfc /scannow` 会还原，风险明显高于 System32（那个本来不存在、只是新增）。
  真遇到 32 位程序拦不到、又想拦，再单独处理。
- 需要**管理员权限**（自动部署会自己弹 UAC）。
- 复制时不要覆盖/替换 `C:\Windows\explorer.exe`（那是系统真正的资源管理器，不在 System32）。
- 杀软可能报“系统目录出现同名程序”，需要加白名单。这是该方案固有的代价。

编译命令（需要单独编这个工程，它不在 Explorer.sln 里）：

```
MSBuild Launcher.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64
MSBuild Launcher.vcxproj /t:Build /p:Configuration=Release /p:Platform=Win32
```

编完之后如果内嵌要跟着更新，跑一次：

```
pwsh -File tools\update-proxy-payload.ps1          # 复制现有产物到 res
pwsh -File tools\update-proxy-payload.ps1 -Build   # 先编 x64 代理再复制
```

## 怎么确认生效

代理会把每次调用写进日志：

```
%LOCALAPPDATA%\Yexplorer\launcher.log
```

（如果调用方是 SYSTEM 账户、拿不到 LOCALAPPDATA，会退到 `%TEMP%\Yexp\launcher.log` 或 `C:\Windows\Temp`）

日志每行一条，两种前缀：

- `[YEXPLORER]`：已交给本文件管理器，后面跟着收到的命令行、识别出的路径、新进程 pid；
- `[SYSTEM]`：已直通系统资源管理器，后面跟着原因（无路径参数 / 没有登记默认管理器 / 路径不存在 等）。

看到 `[YEXPLORER]` 就说明分流成功；如果第三方程序仍然弹出系统资源管理器、而日志里没有新行，
说明它**没有启动 explorer.exe 进程**（见下面“拦不到的情况”）。

## 回退

删掉这两个文件即可，立刻恢复原样：

```
C:\Windows\System32\explorer.exe
C:\Windows\SysWOW64\explorer.exe
```

不需要改任何注册表。

## 拦不到的情况（已知限制）

1. **调用方用 COM 而不是启动进程**：
   例如 `SHOpenFolderAndSelectItems` / `IFolderView2::SelectItem`，它让**已经在运行的**
   资源管理器打开窗口，根本不产生新的 `explorer.exe` 进程，代理没有机会介入。
   这种只能靠替换整个 shell 才能拦，代价太大，不做。
2. **调用方用完整路径** `C:\Windows\explorer.exe`：代理命中不了，也不会介入。
3. **调用方自己拼了别的启动方式**（ShellExecute 的 open 动词、`cmd /c start` 等）：
   前者本来就走默认管理器注册，不需要代理；后者会命中代理（`start` 也是按名字找）。

## 自测记录（本轮，未放到系统目录）

直接运行 `bin\launcher\{x64,x86}\explorer.exe`，用和批量打印软件完全一致的命令行：

| 命令行 | 结果 |
|---|---|
| `/select,"C:\git\explorer\README.md"`（x64） | `[YEXPLORER]`，本程序实例 1，路径识别正确 |
| `/select,"C:\git\explorer\README.md"`（x86） | `[YEXPLORER]`，32 位代理能正常拉起 64 位主程序 |
| `"C:\git"`（纯目录） | `[YEXPLORER]`，打开该目录 |
| `/e,C:\git`（资源管理器开关） | `[SYSTEM]`，本程序实例 0，系统资源管理器接管 |

日志见 `%LOCALAPPDATA%\Yexplorer\launcher.log`；`dumpbin /dependents` 确认只依赖系统 dll。
**放到 System32/SysWOW64 后的真实场景（微信、批量打印软件）由用户手工验证。**
