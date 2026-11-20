# ============================================================================
# 文件管理器（自研 Explorer 替代品）
# 文件：tools/e2e-mainwindow.ps1
#
# 主窗口两个小修复的端到端测试（不需要真实鼠标/键盘）：
#
#   用例 1（A2）：临时解压目录被清理
#     程序停在 %TEMP%\Yexp\...\ 下的目录里时，把这个目录删掉再按 F5：
#     期望 —— 弹一次「临时目录已清理」提示、退回一个还存在的上级目录、
#     日志里只有一条「枚举失败」（不反复枚举、不刷 0x80070002）。
#
#   用例 2（A3）：多选打开失败合并提示
#     打开一个 zip 的列表后把 zip 改坏，全选再「打开」：
#     期望 —— 只弹一条汇总提示（最多列 8 项），不是每项一个框；
#     日志里每项仍有各自的失败记录。
#
# 做法和 tools/e2e-batch-rename.ps1 一样：PostMessage/GetDlgItem + 读日志；
# 弹窗用 FindWindow("#32770") 找，正文用 WM_GETTEXT 读（系统会封送字符串）。
#
# 用法（仓库根目录，需要先编译 Release）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\e2e-mainwindow.ps1
#   退出码 0 = 两个用例都通过。
#
# 注意：
#   · 会临时在 %TEMP%\Yexp\ 下建一个测试目录（结束后删掉）；
#   · %APPDATA%\FileExplorer\settings.json 是用户真实数据，脚本备份后还原；
#   · 测试期间会启动/关闭一个 FileExplorer 实例。
# ============================================================================

[CmdletBinding()]
param(
    [string]$Exe = ''
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrEmpty($Exe)) {
    $Exe = Join-Path (Split-Path -Parent $PSScriptRoot) 'bin\x64\Release\FileExplorer.exe'
}

if (-not (Test-Path -LiteralPath $Exe)) {
    throw "找不到要测试的 exe：$Exe（先编译 Release）"
}

$Exe = (Resolve-Path -LiteralPath $Exe).Path
$LogPath = Join-Path (Split-Path -Parent $Exe) 'FileExplorer.log'

# ----------------------------------------------------------------------------
# Win32 互操作（两个用例共用）
# ----------------------------------------------------------------------------
Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class YexpMainWindowE2E
{
    private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lParam);
    [DllImport("user32.dll")] private static extern bool EnumChildWindows(IntPtr parent, EnumWindowsProc cb, IntPtr lParam);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassNameW(IntPtr hWnd, StringBuilder sb, int max);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowTextW(IntPtr hWnd, StringBuilder sb, int max);
    [DllImport("user32.dll")] private static extern bool PostMessageW(IntPtr hWnd, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] private static extern IntPtr SendMessageW(IntPtr hWnd, uint msg, IntPtr wp, IntPtr lp);

    private const uint WM_GETTEXT = 0x000D;
    private const uint WM_CLOSE = 0x0010;
    private const uint WM_COMMAND = 0x0111;

    public static IntPtr FindWindow(int pid, string cls)
    {
        IntPtr result = IntPtr.Zero;

        EnumWindows(delegate(IntPtr h, IntPtr l)
        {
            uint p;
            GetWindowThreadProcessId(h, out p);
            if (p != (uint)pid) return true;

            StringBuilder sb = new StringBuilder(256);
            GetClassNameW(h, sb, sb.Capacity);

            if (string.Equals(sb.ToString(), cls, StringComparison.OrdinalIgnoreCase))
            {
                result = h;
                return false;
            }

            return true;
        }, IntPtr.Zero);

        return result;
    }

    public static IntPtr WaitWindow(int pid, string cls, int timeoutMs)
    {
        Stopwatch w = Stopwatch.StartNew();

        while (w.ElapsedMilliseconds < timeoutMs)
        {
            IntPtr h = FindWindow(pid, cls);
            if (h != IntPtr.Zero) return h;
            Thread.Sleep(100);
        }

        return IntPtr.Zero;
    }

    public static bool WindowExists(int pid, string cls)
    {
        return FindWindow(pid, cls) != IntPtr.Zero;
    }

    public static string WindowTitle(IntPtr hwnd)
    {
        StringBuilder sb = new StringBuilder(1024);
        GetWindowTextW(hwnd, sb, sb.Capacity);
        return sb.ToString();
    }

    public static string ControlText(IntPtr hwnd)
    {
        IntPtr buffer = Marshal.AllocHGlobal(16384);

        try
        {
            SendMessageW(hwnd, WM_GETTEXT, (IntPtr)8191, buffer);
            return Marshal.PtrToStringUni(buffer) ?? string.Empty;
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    public static string MessageBoxText(IntPtr box)
    {
        StringBuilder all = new StringBuilder();

        EnumChildWindows(box, delegate(IntPtr h, IntPtr l)
        {
            StringBuilder sb = new StringBuilder(64);
            GetClassNameW(h, sb, sb.Capacity);

            if (string.Equals(sb.ToString(), "Static", StringComparison.OrdinalIgnoreCase))
            {
                string text = ControlText(h);
                if (text.Length > 0)
                {
                    all.Append(text);
                    all.Append("\n");
                }
            }

            return true;
        }, IntPtr.Zero);

        return all.ToString().Trim();
    }

    public static void CloseMessageBox(IntPtr box)
    {
        PostMessageW(box, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
    }

    public static void PostCommand(IntPtr window, int id)
    {
        PostMessageW(window, WM_COMMAND, (IntPtr)id, IntPtr.Zero);
    }
}
'@

# ----------------------------------------------------------------------------
# 小工具
# ----------------------------------------------------------------------------
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
        if ((Read-LogText) -match $pattern) {
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

function Close-MessageBoxAndWait($process, $box) {
    [YexpMainWindowE2E]::CloseMessageBox($box)

    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt 5000 -and
           [YexpMainWindowE2E]::WindowExists($process.Id, '#32770')) {
        Start-Sleep -Milliseconds 100
    }
}

# ----------------------------------------------------------------------------
# 用例 1：临时解压目录被清理 -> 回退上级目录、只提示一次
# ----------------------------------------------------------------------------
function Invoke-TempDirGoneCase {
    Write-Host ''
    Write-Host '== 用例 1：临时解压目录被清理（A2）=='

    # 必须建在程序的临时根（%TEMP%\Yexp\）下面，A2 的回退逻辑只认这个根
    $root = Join-Path $env:TEMP 'Yexp\e2e-temp-gone'
    $viewing = Join-Path $root 'cache-sub'

    if (Test-Path -LiteralPath $root) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }

    New-Item -ItemType Directory -Path $viewing -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $viewing 'inner.txt') -Value 'x' -Encoding utf8

    if (Test-Path -LiteralPath $LogPath) {
        Remove-Item -LiteralPath $LogPath -Force
    }

    $process = $null
    $ok = $false

    try {
        $process = Start-Process -FilePath $Exe -ArgumentList ('"' + $viewing + '"') -PassThru

        $main = [YexpMainWindowE2E]::WaitWindow($process.Id, 'FileExplorerMainWindow', 15000)
        if ($main -eq [IntPtr]::Zero) {
            throw '主窗口没出来'
        }

        if (-not (Wait-LogPattern '枚举完成：路径=' 15000)) {
            throw '等不到目录枚举完成'
        }

        Write-Host ('  删除前标题：' + [YexpMainWindowE2E]::WindowTitle($main))

        # 删掉正在浏览的临时目录，再按 F5
        Remove-Item -LiteralPath $root -Recurse -Force
        [YexpMainWindowE2E]::PostCommand($main, 3105)   # F5

        $box = [YexpMainWindowE2E]::WaitWindow($process.Id, '#32770', 10000)
        if ($box -eq [IntPtr]::Zero) {
            throw '没有出现「临时目录已清理」提示框'
        }

        $boxText = [YexpMainWindowE2E]::MessageBoxText($box)
        Write-Host ('  提示正文：' + ($boxText -replace "`r?`n", ' '))

        Close-MessageBoxAndWait $process $box
        Start-Sleep -Seconds 2

        $titleAfter = [YexpMainWindowE2E]::WindowTitle($main)
        Write-Host ('  回退后标题：' + $titleAfter)

        $logText = Read-LogText
        $failCount  = ([regex]::Matches($logText, '枚举失败，路径=')).Count
        $fallback   = ([regex]::Matches($logText, '临时解压目录已经不存在，返回上级目录')).Count

        # 再等 2 秒：如果还在反复枚举，失败条数会继续涨
        Start-Sleep -Seconds 2
        $failCount2 = ([regex]::Matches((Read-LogText), '枚举失败，路径=')).Count

        Write-Host ("  枚举失败 $failCount 条（2 秒后 $failCount2 条），回退日志 $fallback 条")

        $ok = ($boxText -match '临时解压目录') -and
              ($titleAfter -notlike '*cache-sub*') -and
              ($fallback -ge 1) -and
              ($failCount -ge 1) -and
              ($failCount2 -le $failCount)
    }
    finally {
        Stop-TestProcess $process
        if (Test-Path -LiteralPath $root) {
            Remove-Item -LiteralPath $root -Recurse -Force
        }
    }

    if ($ok) {
        Write-Host '  通过' -ForegroundColor Green
    } else {
        Write-Host '  失败' -ForegroundColor Red
    }

    return $ok
}

# ----------------------------------------------------------------------------
# 用例 2：多选打开失败合并提示
# ----------------------------------------------------------------------------
function Invoke-BatchOpenFailureCase {
    Write-Host ''
    Write-Host '== 用例 2：多选打开失败合并提示（A3）=='

    $work = Join-Path $env:TEMP 'yexp-a3-e2e'
    $src  = Join-Path $work 'src'
    $zip  = Join-Path $work 'many-files.zip'

    if (Test-Path -LiteralPath $work) {
        Remove-Item -LiteralPath $work -Recurse -Force
    }

    New-Item -ItemType Directory -Path $src -Force | Out-Null

    for ($i = 1; $i -le 8; ++$i) {
        Set-Content -LiteralPath (Join-Path $src ("file{0}.txt" -f $i)) -Value ("content {0}" -f $i) -Encoding utf8
    }

    Compress-Archive -Path (Join-Path $src '*') -DestinationPath $zip -Force

    if (Test-Path -LiteralPath $LogPath) {
        Remove-Item -LiteralPath $LogPath -Force
    }

    $process = $null
    $ok = $false

    try {
        $process = Start-Process -FilePath $Exe -ArgumentList ('"' + $zip + '"') -PassThru

        $main = [YexpMainWindowE2E]::WaitWindow($process.Id, 'FileExplorerMainWindow', 15000)
        if ($main -eq [IntPtr]::Zero) {
            throw '主窗口没出来'
        }

        if (-not (Wait-LogPattern '条目数=8' 15000)) {
            throw '没有把 zip 里的 8 项列出来'
        }

        Write-Host ('  列表标题：' + [YexpMainWindowE2E]::WindowTitle($main))

        # 把 zip 改坏（截断成 64 字节垃圾），这样每项解压都会失败
        [System.IO.File]::WriteAllBytes($zip, (New-Object byte[] 64))

        [YexpMainWindowE2E]::PostCommand($main, 3107)   # 全选
        Start-Sleep -Milliseconds 400
        [YexpMainWindowE2E]::PostCommand($main, 3201)   # 打开

        $box = [YexpMainWindowE2E]::WaitWindow($process.Id, '#32770', 15000)
        if ($box -eq [IntPtr]::Zero) {
            throw '没有出现汇总失败提示'
        }

        Start-Sleep -Milliseconds 300
        $boxText = [YexpMainWindowE2E]::MessageBoxText($box)
        Write-Host '  --- 提示正文 ---'
        foreach ($line in ($boxText -split "`r?`n")) {
            Write-Host ('  ' + $line)
        }
        Write-Host '  ----------------'

        Close-MessageBoxAndWait $process $box

        # 旧的逐项弹框行为下，关掉一个马上又弹下一个
        $second = [YexpMainWindowE2E]::WindowExists($process.Id, '#32770')

        $logText = Read-LogText
        $failedLogs = ([regex]::Matches($logText, '提取容器内部文件失败')).Count
        $mergedLogs = ([regex]::Matches($logText, '已合并提示')).Count

        Write-Host ("  单项失败日志 $failedLogs 条，合并提示日志 $mergedLogs 条，关掉后还有弹窗：$second")

        $ok = ($boxText -match '有 8 项打开失败') -and
              ($boxText -match 'file1\.txt') -and
              (-not $second) -and
              ($failedLogs -ge 8) -and
              ($mergedLogs -ge 1)
    }
    finally {
        Stop-TestProcess $process
        if (Test-Path -LiteralPath $work) {
            Remove-Item -LiteralPath $work -Recurse -Force
        }
    }

    if ($ok) {
        Write-Host '  通过' -ForegroundColor Green
    } else {
        Write-Host '  失败' -ForegroundColor Red
    }

    return $ok
}

# ----------------------------------------------------------------------------
# 主流程
# ----------------------------------------------------------------------------
$settingsPath = Join-Path $env:APPDATA 'FileExplorer\settings.json'
$backupPath   = Join-Path $env:TEMP 'yexp-mainwindow-settings-backup.json'
$hadSettings  = Test-Path -LiteralPath $settingsPath

if ($hadSettings) {
    Copy-Item -LiteralPath $settingsPath -Destination $backupPath -Force
}

Write-Host "被测程序：$Exe"

try {
    $results = @()
    $results += Invoke-TempDirGoneCase
    $results += Invoke-BatchOpenFailureCase
}
finally {
    if ($hadSettings -and (Test-Path -LiteralPath $backupPath)) {
        Copy-Item -LiteralPath $backupPath -Destination $settingsPath -Force
    }
}

if ($results -contains $false -or $results.Count -ne 2) {
    Write-Host ''
    Write-Host '主窗口端到端测试失败' -ForegroundColor Red
    exit 1
}

Write-Host ''
Write-Host '主窗口端到端测试全部通过' -ForegroundColor Green
exit 0
