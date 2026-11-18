# ============================================================================
# 「此电脑」看不到本地硬盘 —— 诊断脚本
#
# 用法（在最右边那台有问题的电脑上）：
#   1. 把这个 ps1 和同目录的 diagnose-thispc.bat 一起拷过去；
#   2. 双击 diagnose-thispc.bat（它用 pwsh / powershell 跑，窗口会停在结果上）；
#   3. 把窗口里全部文字复制回来。
#
# 它只读不写：枚举盘符、问 Shell 要「此电脑」里的项、读两条显示策略、看程序日志。
# ============================================================================

$ErrorActionPreference = 'Continue'

function Show($text) { Write-Host $text }

Show "======================================================"
Show " 1) 系统层面到底有没有盘（Win32_LogicalDisk）"
Show "======================================================"
try {
    Get-CimInstance -ClassName Win32_LogicalDisk |
        Select-Object DeviceID, DriveType, VolumeName, ProviderName, @{n='GB';e={[math]::Round($_.Size/1GB,1)}} |
        Format-Table -AutoSize | Out-String | Write-Host
} catch {
    Show ("查询失败：" + $_.Exception.Message)
}

Show "======================================================"
Show " 2) Shell 的「此电脑」里能枚举出哪些项（程序就是照这个显示）"
Show "======================================================"
try {
    $shell  = New-Object -ComObject Shell.Application
    $folder = $shell.Namespace('::{20D04FE0-3AEA-1069-A2D8-08002B30309D}')

    if ($null -eq $folder) {
        Show "！！此电脑命名空间打不开（Shell 自己都没有这个文件夹）"
    } else {
        $count = 0
        foreach ($item in $folder.Items()) {
            $path = ''
            try { $path = $folder.GetDetailsOf($item, 194) } catch { }   # 194 = 解析名一列
            if (-not $path) { try { $path = $item.Path } catch { } }

            $name = ''
            try { $name = $item.Name } catch { }

            Show ("  [{0}] 名字='{1}'  类型='{2}'  路径='{3}'" -f $count, $name, $item.Type, $path)
            $count++
        }
        Show ("  共 " + $count + " 项")
        if ($count -eq 0) {
            Show "！！Shell 这一层就返回 0 项：程序看不到盘不是程序的锅，是系统/策略"
        }
    }
} catch {
    Show ("枚举失败：" + $_.Exception.Message)
}

Show "======================================================"
Show " 2b)「主文件夹」（Win11 Home）里能枚举出哪些项（对照用）"
Show "======================================================"
try {
    $shell2  = New-Object -ComObject Shell.Application
    $folder2 = $shell2.Namespace('::{F874310E-B6B7-47DC-BC84-B9E6B38F5903}')

    if ($null -eq $folder2) {
        Show "  （这台机器没有「主文件夹」这个命名空间，正常）"
    } else {
        $count2 = 0
        foreach ($item in $folder2.Items()) {
            if ($count2 -ge 12) { Show "  ...（后面省略）"; break }
            Show ("  [{0}] {1}" -f $count2, $item.Name)
            $count2++
        }
    }
} catch {
    Show ("  枚举失败：" + $_.Exception.Message)
}

Show "======================================================"
Show " 3) 会影响盘符显示的策略 / 设置"
Show "======================================================"
$policies = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Policies\Explorer'
$advanced = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced'
$machine  = 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Policies\Explorer'

$noDrives = (Get-ItemProperty -Path $policies -Name 'NoDrives' -ErrorAction SilentlyContinue).NoDrives
if ($null -eq $noDrives) { Show "  NoDrives                = 未设置（正常）" }
else                     { Show ("  NoDrives                = " + $noDrives + "  ← 非 0 会把对应盘符藏起来") }

$noViewOnDrive = (Get-ItemProperty -Path $policies -Name 'NoViewOnDrive' -ErrorAction SilentlyContinue).NoViewOnDrive
if ($null -eq $noViewOnDrive) { Show "  NoViewOnDrive           = 未设置（正常）" }
else                          { Show ("  NoViewOnDrive           = " + $noViewOnDrive) }

$hideNoMedia = (Get-ItemProperty -Path $advanced -Name 'HideDrivesWithNoMedia' -ErrorAction SilentlyContinue).HideDrivesWithNoMedia
if ($null -eq $hideNoMedia) { Show "  HideDrivesWithNoMedia   = 未设置（正常）" }
else                        { Show ("  HideDrivesWithNoMedia   = " + $hideNoMedia) }

$policyMachine = (Get-ItemProperty -Path $machine -Name 'NoDrives' -ErrorAction SilentlyContinue).NoDrives
if ($null -eq $policyMachine) { Show "  机器级 NoDrives         = 未设置（正常）" }
else                          { Show ("  机器级 NoDrives         = " + $policyMachine) }

Show "======================================================"
Show " 4) 程序日志（FileExplorer.log）里跟「此电脑」有关的部分"
Show "======================================================"
$candidates = @(
    (Join-Path $PSScriptRoot 'FileExplorer.log'),
    (Join-Path (Split-Path $PSScriptRoot -Parent) 'bin\x64\Release\FileExplorer.log'),
    (Join-Path (Split-Path $PSScriptRoot -Parent) 'bin\x64\Debug\FileExplorer.log'),
    (Join-Path (Split-Path $PSScriptRoot -Parent) 'FileExplorer.log')
)

$found = $false
foreach ($log in $candidates) {
    if (Test-Path -LiteralPath $log) {
        $found = $true
        Show ("  日志文件：" + $log)
        $lines = Select-String -LiteralPath $log -Pattern '此电脑|设备和驱动器|网络位置|EnumObjects|枚举' -Encoding utf8 |
                 Select-Object -Last 40
        if ($null -eq $lines) { Show "  （没有匹配的行）" }
        else { foreach ($line in $lines) { Show ("  " + $line.Line) } }
    }
}

if (-not $found) {
    Show "  没找到日志。请在程序目录（FileExplorer.exe 旁边）找 FileExplorer.log，"
    Show "  或者把 Yexplorer 打开一次「此电脑」再运行本脚本。"
}

Show "======================================================"
Show " 5) 其它信息"
Show "======================================================"
Show ("  OS        : " + (Get-CimInstance Win32_OperatingSystem).Caption)
Show ("  版本      : " + (Get-CimInstance Win32_OperatingSystem).Version)
Show ("  PowerShell: " + $PSVersionTable.PSVersion)
Show "完成。把上面全部内容复制发回即可。"
