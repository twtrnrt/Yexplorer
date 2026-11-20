# ============================================================================
# 安装 / 卸载 Yexplorer 的 Listary 插件（独立包用）
#
# 用法：
#   右键“使用 PowerShell 运行”，或在 PowerShell 里：
#     powershell -NoProfile -ExecutionPolicy Bypass -File .\install-listary-plugin.ps1
#   -Uninstall 卸载。
#
# 说明：接口 DLL（Listary.FileAppPlugin.dll 等）从本机已安装的 Listary 复制，
# 不随包重新分发。
# ============================================================================
param(
    [switch]$Uninstall
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $PSCommandPath

function Test-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Find-Listary {
    foreach ($base in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if ([string]::IsNullOrEmpty($base)) { continue }
        $candidate = Join-Path $base "Listary"
        if (Test-Path (Join-Path $candidate "Listary.exe")) {
            return $candidate
        }
    }
    return $null
}

$listary = Find-Listary
if (-not $listary) {
    Write-Host "没有找到 Listary（默认在 Program Files\Listary）。请先安装 Listary。" -ForegroundColor Red
    exit 1
}

$target = Join-Path $listary "FileAppPlugins\Yexplorer"

if (-not (Test-Admin)) {
    Write-Host "需要管理员权限，正在重新以管理员身份启动 …"
    Start-Process powershell.exe -Verb RunAs -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $PSCommandPath,
        $(if ($Uninstall) { "-Uninstall" } else { "" })
    )
    exit
}

if ($Uninstall) {
    if (Test-Path $target) {
        Remove-Item -LiteralPath $target -Recurse -Force
        Write-Host "已卸载：$target（重启 Listary 生效）"
    } else {
        Write-Host "插件没有安装：$target"
    }
    exit
}

New-Item -ItemType Directory -Path $target -Force | Out-Null

Copy-Item -LiteralPath (Join-Path $here "Listary.FileAppPlugin.Yexplorer.dll") -Destination $target -Force
Copy-Item -LiteralPath (Join-Path $here "ListaryPluginManifest.json") -Destination $target -Force
if (Test-Path (Join-Path $here "Icon.png")) {
    Copy-Item -LiteralPath (Join-Path $here "Icon.png") -Destination $target -Force
}

# 从本机 Listary 的官方插件目录取接口依赖，避免重新分发 Listary 的文件
$reference = Join-Path $listary "FileAppPlugins\Files"
foreach ($name in @("Listary.FileAppPlugin.dll",
                    "Microsoft.Extensions.Logging.Abstractions.dll",
                    "System.Buffers.dll",
                    "System.Memory.dll",
                    "System.Numerics.Vectors.dll",
                    "System.Runtime.CompilerServices.Unsafe.dll")) {
    $source = Join-Path $reference $name
    if (Test-Path $source) {
        Copy-Item -LiteralPath $source -Destination $target -Force
    }
}

Write-Host "已安装：$target"
Write-Host "请在任务栏托盘退出 Listary 再重新打开，然后测试："
Write-Host "  1. 用 Yexplorer 打开一个目录；"
Write-Host "  2. 打开任意保存/打开对话框；"
Write-Host "  3. 按 Ctrl+G（或先点 Yexplorer 再切回对话框）。"
