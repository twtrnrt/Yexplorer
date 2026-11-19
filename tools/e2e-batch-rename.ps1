# ============================================================================
# 文件管理器（自研 Explorer 替代品）
# 文件：tools/e2e-batch-rename.ps1
#
# 批量重命名的端到端自动测试（不需要真实鼠标/键盘）。
#
# 为什么要有它：
#   批量重命名的规则引擎经过 C8-1 重构（ComputeNewName 不再直接读控件），
#   光有纯函数回归还不够 —— 得证明"真实窗口里的控件 -> CaptureCurrentRules ->
#   ComputeNewName -> 落盘改名"整条链路还是对的。
#
# 做法（和 docs/交接-2026-11-19.md 第 8 节一致）：
#   1. 造临时目录，启动 bin\x64\Release\FileExplorer.exe 指向该目录；
#   2. 向主窗口 PostMessage(WM_COMMAND, 3107) 全选、3211 打开批量重命名；
#   3. 用 SendMessage/GetDlgItem 按控件 ID 填规则（文本走 WM_SETTEXT，系统会封送字符串）；
#   4. 发 WM_COMMAND 点「应用」，等窗口关闭，检查磁盘上的文件名和日志。
#
# 用例只走"点应用 = 真的改名"这条真实路径；预览表里的非法名字用例
# （重名 / 非法字符 / 正则写错）不在这里测，它们只做内存计算，
# 由 tests/BatchRenameRulesTest.cpp 对重构前基准逐条核对。
#
# 用法（仓库根目录）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\e2e-batch-rename.ps1
#   结束时退出码 0 = 全部通过。
#
# 注意：
#   · 会启动/关闭一个 FileExplorer 实例，测试目录在 %TEMP%\yexp-e2e*；
#   · %APPDATA%\FileExplorer\settings.json 是用户真实数据，本脚本开头备份、结束还原；
#   · 日志文件在 exe 同目录（bin\x64\Release\FileExplorer.log），每次用例前清掉。
# ============================================================================

[CmdletBinding()]
param(
    [string]$Exe = '',
    [string]$Root = ''
)

# 注意：$PSScriptRoot 不能写在 param 默认值里 —— 带 [CmdletBinding()] 的脚本在
# Windows PowerShell 5.1 里绑参数时它还是空的（实测），只能进了脚本再取。
if ([string]::IsNullOrEmpty($Exe)) {
    $Exe = Join-Path (Split-Path -Parent $PSScriptRoot) 'bin\x64\Release\FileExplorer.exe'
}

if ([string]::IsNullOrEmpty($Root)) {
    $Root = Join-Path $env:TEMP 'yexp-e2e'
}

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Exe)) {
    throw "找不到要测试的 exe：$Exe（先编译 Release）"
}

$Exe = (Resolve-Path -LiteralPath $Exe).Path
$ExeDir = Split-Path -Parent $Exe
$LogPath = Join-Path $ExeDir 'FileExplorer.log'

# ----------------------------------------------------------------------------
# Win32 互操作（C# 写在脚本里，Add-Type 编译一次）
# ----------------------------------------------------------------------------
Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class YexpE2E
{
    private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassNameW(IntPtr hWnd, StringBuilder text, int maxCount);

    [DllImport("user32.dll")]
    private static extern bool PostMessageW(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern IntPtr SendMessageW(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern IntPtr GetDlgItem(IntPtr dialog, int id);

    private const uint WM_SETTEXT = 0x000C;
    private const uint WM_GETTEXT = 0x000D;
    private const uint WM_CLOSE = 0x0010;
    private const uint WM_COMMAND = 0x0111;
    private const uint BM_SETCHECK = 0x00F1;
    private const uint CB_SETCURSEL = 0x014E;
    private const int BST_CHECKED = 1;

    public static IntPtr WaitWindow(int processId, string className, int timeoutMs)
    {
        Stopwatch watch = Stopwatch.StartNew();

        while (watch.ElapsedMilliseconds < timeoutMs)
        {
            IntPtr found = FindWindow(processId, className);
            if (found != IntPtr.Zero)
            {
                return found;
            }

            Thread.Sleep(100);
        }

        return IntPtr.Zero;
    }

    public static bool WaitWindowGone(int processId, string className, int timeoutMs)
    {
        Stopwatch watch = Stopwatch.StartNew();

        while (watch.ElapsedMilliseconds < timeoutMs)
        {
            if (FindWindow(processId, className) == IntPtr.Zero)
            {
                return true;
            }

            Thread.Sleep(100);
        }

        return FindWindow(processId, className) == IntPtr.Zero;
    }

    public static bool WindowExists(int processId, string className)
    {
        return FindWindow(processId, className) != IntPtr.Zero;
    }

    private static IntPtr FindWindow(int processId, string className)
    {
        IntPtr result = IntPtr.Zero;

        EnumWindows(delegate(IntPtr hWnd, IntPtr lParam)
        {
            uint pid;
            GetWindowThreadProcessId(hWnd, out pid);

            if (pid != (uint)processId)
            {
                return true;
            }

            StringBuilder text = new StringBuilder(256);
            GetClassNameW(hWnd, text, text.Capacity);

            if (string.Equals(text.ToString(), className, StringComparison.OrdinalIgnoreCase))
            {
                result = hWnd;
                return false;
            }

            return true;
        }, IntPtr.Zero);

        return result;
    }

    public static void PostCommand(IntPtr window, int commandId)
    {
        PostMessageW(window, WM_COMMAND, (IntPtr)commandId, IntPtr.Zero);
    }

    public static void PostClose(IntPtr window)
    {
        PostMessageW(window, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
    }

    private static IntPtr Child(IntPtr dialog, int id)
    {
        IntPtr child = GetDlgItem(dialog, id);
        if (child == IntPtr.Zero)
        {
            throw new Exception("对话框里找不到控件 ID " + id);
        }

        return child;
    }

    // 跨进程发 WM_SETTEXT / WM_GETTEXT：这两条是 USER32 帮我们封送（marshal）的系统消息，
    // 传【本进程】的缓冲区地址即可（实测传目标进程的远程地址反而读不到）。
    public static void SetText(IntPtr dialog, int id, string value)
    {
        IntPtr child = Child(dialog, id);
        IntPtr buffer = Marshal.StringToHGlobalUni(value);

        try
        {
            SendMessageW(child, WM_SETTEXT, IntPtr.Zero, buffer);
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    public static string GetText(IntPtr dialog, int id)
    {
        IntPtr child = Child(dialog, id);
        IntPtr buffer = Marshal.AllocHGlobal(4096);

        try
        {
            SendMessageW(child, WM_GETTEXT, (IntPtr)2047, buffer);
            return Marshal.PtrToStringUni(buffer) ?? string.Empty;
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    public static bool HasMessageBox(int processId)
    {
        // TaskDialog / MessageBox 的窗口类都是 #32770
        return FindWindow(processId, "#32770") != IntPtr.Zero;
    }

    public static void SelectCombo(IntPtr dialog, int id, int index)
    {
        SendMessageW(Child(dialog, id), CB_SETCURSEL, (IntPtr)index, IntPtr.Zero);
    }

    public static void Check(IntPtr dialog, int id, bool on)
    {
        SendMessageW(Child(dialog, id), BM_SETCHECK, (IntPtr)(on ? BST_CHECKED : 0), IntPtr.Zero);
    }

    public static void TriggerRecalculate(IntPtr dialog)
    {
        // 规则区的任何 EN_CHANGE 都会让窗口重算（前缀 5001）；这里只是把"填完值"通知到位
        int wParam = (0x0300 << 16) | 5001;
        SendMessageW(dialog, WM_COMMAND, (IntPtr)wParam, IntPtr.Zero);
    }

    public static void ClickApply(IntPtr dialog)
    {
        // 自绘按钮：ID 5102、BN_CLICKED(0)，lParam 必须为 0。
        // 用 PostMessage 而不是 SendMessage：万一弹了异常提示，SendMessage 会卡在对方的模态循环里。
        int wParam = (0 << 16) | 5102;
        PostMessageW(dialog, WM_COMMAND, (IntPtr)wParam, IntPtr.Zero);
    }

    public static void Release()
    {
        // 现在没有跨进程资源要释放；保留这个入口，脚本的 finally 里统一调用。
    }
}
'@

# ----------------------------------------------------------------------------
# 小工具
# ----------------------------------------------------------------------------

# 用 UTF-8 + FileShare.ReadWrite 读日志（程序还开着的时候也能读）
function Read-LogText {
    if (-not (Test-Path -LiteralPath $LogPath)) {
        return ''
    }

    $stream = [System.IO.FileStream]::new($LogPath, [System.IO.FileMode]::Open,
                                          [System.IO.FileAccess]::Read,
                                          [System.IO.FileShare]::ReadWrite)
    try {
        $reader = [System.IO.StreamReader]::new($stream, [System.Text.Encoding]::UTF8)
        return $reader.ReadToEnd()
    }
    finally {
        if ($null -ne $reader) { $reader.Dispose() }
        $stream.Dispose()
    }
}

function Wait-LogPattern([string]$pattern, [int]$timeoutMs) {
    $watch = [System.Diagnostics.Stopwatch]::StartNew()

    while ($watch.ElapsedMilliseconds -lt $timeoutMs) {
        $text = Read-LogText
        if ($text -match $pattern) {
            return $true
        }

        Start-Sleep -Milliseconds 100
    }

    return $false
}

function Stop-TestProcess($process) {
    if ($null -eq $process) { return }

    try {
        if (-not $process.HasExited) {
            $process.CloseMainWindow() | Out-Null
            if (-not $process.WaitForExit(5000)) {
                $process.Kill()
                $process.WaitForExit(3000) | Out-Null
            }
        }
    }
    catch {
        try { $process.Kill() } catch { }
    }
}

function New-TestDirectory([string]$path, [string[]]$names) {
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Recurse -Force
    }

    New-Item -ItemType Directory -Path $path -Force | Out-Null

    foreach ($name in $names) {
        Set-Content -LiteralPath (Join-Path $path $name) -Value 'e2e' -Encoding utf8 -NoNewline
    }
}

# 一个用例：起程序 -> 全选 -> 批量重命名 -> 填规则 -> 应用 -> 核对磁盘
function Invoke-BatchRenameCase {
    param(
        [string]$Name,
        [string[]]$FileNames,
        [hashtable]$Rules,
        [string[]]$ExpectedNames
    )

    Write-Host ""
    Write-Host "== 用例：$Name =="

    $dir = Join-Path $Root $Name
    New-TestDirectory $dir $FileNames

    if (Test-Path -LiteralPath $LogPath) {
        Remove-Item -LiteralPath $LogPath -Force
    }

    $process = $null
    $dialog = [IntPtr]::Zero

    try {
        $process = Start-Process -FilePath $Exe -ArgumentList ('"' + $dir + '"') -PassThru

        $main = [YexpE2E]::WaitWindow($process.Id, 'FileExplorerMainWindow', 15000)
        if ($main -eq [IntPtr]::Zero) {
            throw '主窗口没出来'
        }

        if (-not (Wait-LogPattern '枚举完成' 15000)) {
            throw '等不到目录枚举完成'
        }

        # 全选 -> 批量重命名
        [YexpE2E]::PostCommand($main, 3107)
        Start-Sleep -Milliseconds 300
        [YexpE2E]::PostCommand($main, 3211)

        $dialog = [YexpE2E]::WaitWindow($process.Id, 'FileExplorerBatchRename', 15000)
        if ($dialog -eq [IntPtr]::Zero) {
            throw '批量重命名窗口没出来'
        }

        # 等预览表把行数填好（窗口创建后马上就会算一遍，这里只是给一点余量）
        Start-Sleep -Milliseconds 500

        foreach ($text in $Rules.Texts.GetEnumerator()) {
            [YexpE2E]::SetText($dialog, [int]$text.Key, [string]$text.Value)
        }
        foreach ($combo in $Rules.Combos.GetEnumerator()) {
            [YexpE2E]::SelectCombo($dialog, [int]$combo.Key, [int]$combo.Value)
        }
        foreach ($check in $Rules.Checks.GetEnumerator()) {
            [YexpE2E]::Check($dialog, [int]$check.Key, [bool]$check.Value)
        }

        [YexpE2E]::TriggerRecalculate($dialog)
        Start-Sleep -Milliseconds 300

        # 填完先读回来核对：WM_SETTEXT 是系统帮忙封送的，没填进去的话后面会算错名字
        foreach ($text in $Rules.Texts.GetEnumerator()) {
            $actualText = [YexpE2E]::GetText($dialog, [int]$text.Key)
            if ($actualText -ne [string]$text.Value) {
                throw ("控件 {0} 填值没生效：期望「{1}」，实际「{2}」" -f $text.Key, $text.Value, $actualText)
            }
        }

        [YexpE2E]::ClickApply($dialog)

        # 等窗口自己关掉；中途冒提示框就说明规则没生效/名字冲突，立刻失败，别卡在模态循环里
        $closed = $false
        $deadline = [DateTime]::UtcNow.AddSeconds(15)

        while ([DateTime]::UtcNow -lt $deadline) {
            if (-not [YexpE2E]::WindowExists($process.Id, 'FileExplorerBatchRename')) {
                $closed = $true
                break
            }

            if ([YexpE2E]::HasMessageBox($process.Id)) {
                throw '点了「应用」之后弹了提示框（规则没生效或文件有问题）'
            }

            Start-Sleep -Milliseconds 100
        }

        if (-not $closed) {
            throw '点了「应用」之后窗口没有关闭'
        }

        # 等主窗口把结果收下、日志写完
        $logOk = Wait-LogPattern '批量重命名：已改名' 5000
        Start-Sleep -Milliseconds 300

        $actual = @(Get-ChildItem -LiteralPath $dir -File | ForEach-Object { $_.Name } | Sort-Object)
        $expected = @($ExpectedNames | Sort-Object)

        $diff = Compare-Object -ReferenceObject $expected -DifferenceObject $actual

        $log = Read-LogText
        $errors = @($log -split "`r?`n" | Where-Object { $_ -match '\[error\]' })

        $ok = $true

        if (-not $logOk) {
            Write-Host '  [失败] 日志里没有「批量重命名：已改名」' -ForegroundColor Red
            $ok = $false
        }

        if ($null -ne $diff) {
            Write-Host '  [失败] 磁盘上的名字和预期不一致：' -ForegroundColor Red
            $diff | ForEach-Object { Write-Host ("         {0} {1}" -f $_.SideIndicator, $_.InputObject) }
            $ok = $false
        }

        if ($errors.Count -gt 0) {
            Write-Host ("  [失败] 日志里有 {0} 条 [error]：{1}" -f $errors.Count, $errors[0]) -ForegroundColor Red
            $ok = $false
        }

        $renamed = @($log -split "`r?`n" | Where-Object { $_ -match '批量重命名：已改名' })
        if ($renamed.Count -gt 0) {
            Write-Host ("  日志：{0}" -f $renamed[-1].Trim())
        }

        if ($ok) {
            Write-Host ("  通过（{0} 个文件）" -f $actual.Count) -ForegroundColor Green
        }

        if (-not $ok) {
            Write-Host ("  测试目录保留在：{0}" -f $dir) -ForegroundColor Yellow
        }

        return $ok
    }
    finally {
        [YexpE2E]::Release()
        Stop-TestProcess $process
    }
}

# ----------------------------------------------------------------------------
# 主流程
# ----------------------------------------------------------------------------

$settingsPath = Join-Path $env:APPDATA 'FileExplorer\settings.json'
$backupPath = Join-Path $env:TEMP 'yexp-e2e-settings-backup.json'
$hadSettings = Test-Path -LiteralPath $settingsPath

if ($hadSettings) {
    Copy-Item -LiteralPath $settingsPath -Destination $backupPath -Force
}

Write-Host "被测程序：$Exe"

$results = @()

try {
    # 用例 1：前缀 + 序号（起始/步长/位数/位置）+ 改扩展名
    $results += Invoke-BatchRenameCase -Name 'prefix-number-ext' -FileNames @(
        'alpha.txt', 'Beta.TXT', 'notes.md', 'photo.jpeg', 'readme', '报告 2024.docx'
    ) -Rules @{
        Texts = @{
            5001 = 'P_'          # 前缀
            5013 = 'dat'         # 扩展名值
            5007 = '7'           # 起始
            5008 = '2'           # 步长
            5009 = '3'           # 位数
            5011 = '-'           # 分隔符
        }
        Combos = @{
            5012 = 1             # 扩展名：修改
            5010 = 1             # 序号位置：放在名字前面
        }
        Checks = @{
            5006 = $true         # 加序号
        }
    } -ExpectedNames @(
        'P_007-alpha.dat',
        'P_009-Beta.dat',
        'P_011-notes.dat',
        'P_013-photo.dat',
        'P_015-readme.dat',
        'P_017-报告 2024.dat'
    )

    # 用例 2：查找替换（含扩展名）+ 后缀 + 区分大小写关
    $results += Invoke-BatchRenameCase -Name 'find-replace-suffix' -FileNames @(
        'alpha.txt', 'Beta.TXT', 'notes.md', 'report.docx', 'readme', '100% (copy).txt'
    ) -Rules @{
        Texts = @{
            5003 = 'a'           # 查找
            5004 = 'X'           # 替换为
            5002 = '_s'          # 后缀
        }
        Combos = @{}
        Checks = @{
            5019 = $true         # 含扩展名
        }
    } -ExpectedNames @(
        'XlphX_s.txt',
        'BetX_s.TXT',
        'notes_s.md',
        'report_s.docx',
        'reXdme_s',
        '100% (copy)_s.txt'
    )
}
finally {
    [YexpE2E]::Release()

    # 还原用户真实配置（程序退出时可能写了窗口位置）
    if ($hadSettings -and (Test-Path -LiteralPath $backupPath)) {
        Copy-Item -LiteralPath $backupPath -Destination $settingsPath -Force
    }
}

if ($results -contains $false -or $results.Count -ne 2) {
    Write-Host ""
    Write-Host '批量重命名端到端测试失败' -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host '批量重命名端到端测试全部通过' -ForegroundColor Green
exit 0
