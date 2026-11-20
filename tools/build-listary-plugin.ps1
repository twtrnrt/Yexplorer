# ============================================================================
# 构建 / 安装 Yexplorer 的 Listary 插件
#
# 用法：
#   powershell -File tools\build-listary-plugin.ps1            # 只构建到 dist\listary-plugin
#   powershell -File tools\build-listary-plugin.ps1 -Install   # 构建并安装（需要一次 UAC）
#   powershell -File tools\build-listary-plugin.ps1 -Uninstall # 卸载（需要一次 UAC）
#
# 说明见 integrations\listary\README.md。
# ============================================================================
param(
    [switch]$Install,
    [switch]$Uninstall
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot

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
    throw "找不到 Listary 安装目录（默认在 Program Files\Listary）"
}

$pluginDir = Join-Path $listary "FileAppPlugins\Yexplorer"

function Invoke-Elevated([string]$command) {
    $process = Start-Process powershell.exe -ArgumentList @("-NoProfile", "-Command", $command) `
                     -Verb RunAs -PassThru -Wait -ErrorAction Stop
    if ($process.ExitCode -ne 0) {
        throw "提权操作失败，退出码 $($process.ExitCode)"
    }
}

if ($Uninstall) {
    if (-not (Test-Path $pluginDir)) {
        Write-Host "插件没有安装：$pluginDir"
        return
    }
    Invoke-Elevated "Remove-Item -LiteralPath '$pluginDir' -Recurse -Force"
    Write-Host "已卸载：$pluginDir（重启 Listary 生效）"
    return
}

$source   = Join-Path $root "integrations\listary\YexplorerPlugin.cs"
$manifest = Join-Path $root "integrations\listary\ListaryPluginManifest.json"
$outDir   = Join-Path $root "dist\listary-plugin"

if (-not (Test-Path $source))   { throw "找不到插件源码：$source" }
if (-not (Test-Path $manifest)) { throw "找不到插件清单：$manifest" }

if (Test-Path $outDir) {
    Remove-Item -LiteralPath $outDir -Recurse -Force
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$csc = Join-Path $env:WINDIR "Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if (-not (Test-Path $csc)) {
    throw "找不到 .NET Framework 编译器：$csc"
}

$refDir = Join-Path $listary "FileAppPlugins\Files"
$interfaceDll = Join-Path $refDir "Listary.FileAppPlugin.dll"
$loggingDll   = Join-Path $refDir "Microsoft.Extensions.Logging.Abstractions.dll"
if (-not (Test-Path $interfaceDll)) { throw "找不到 Listary 插件接口：$interfaceDll" }

Write-Host "编译插件 …"
& $csc /nologo /target:library `
       /out:"$outDir\Listary.FileAppPlugin.Yexplorer.dll" `
       /r:"$interfaceDll" /r:"$loggingDll" `
       $source
if ($LASTEXITCODE -ne 0) {
    throw "插件编译失败，退出码 $LASTEXITCODE"
}

Copy-Item -LiteralPath $manifest -Destination "$outDir\ListaryPluginManifest.json" -Force
Copy-Item -LiteralPath $interfaceDll -Destination $outDir -Force
Copy-Item -LiteralPath $loggingDll -Destination $outDir -Force

foreach ($name in @("System.Buffers.dll", "System.Memory.dll", "System.Numerics.Vectors.dll",
                    "System.Runtime.CompilerServices.Unsafe.dll")) {
    $path = Join-Path $refDir $name
    if (Test-Path $path) {
        Copy-Item -LiteralPath $path -Destination $outDir -Force
    }
}

# 用本程序的图标，避免清单里的 Icon 指向不存在的文件
$exe = Join-Path $root "bin\x64\Release\FileExplorer.exe"
if (Test-Path $exe) {
    try {
        Add-Type -AssemblyName System.Drawing
        $icon = [System.Drawing.Icon]::ExtractAssociatedIcon($exe)
        $icon.ToBitmap().Save((Join-Path $outDir "Icon.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    } catch {
        Write-Warning "生成 Icon.png 失败：$($_.Exception.Message)"
    }
}


# 同步一份到 res\，供主程序内嵌（rc 资源）
Copy-Item -LiteralPath (Join-Path $outDir 'Listary.FileAppPlugin.Yexplorer.dll') -Destination (Join-Path $root 'res\listary-plugin.dll') -Force
Copy-Item -LiteralPath (Join-Path $outDir 'ListaryPluginManifest.json') -Destination (Join-Path $root 'res\listary-plugin-manifest.json') -Force
if (Test-Path (Join-Path $outDir 'Icon.png')) {
    Copy-Item -LiteralPath (Join-Path $outDir 'Icon.png') -Destination (Join-Path $root 'res\listary-plugin-icon.png') -Force
}

Write-Host "构建完成：$outDir"
Get-ChildItem $outDir | Select-Object Name, Length

if ($Install) {
    $command = "New-Item -ItemType Directory -Force -Path '$pluginDir' | Out-Null; " +
               "Copy-Item -Path '$outDir\*' -Destination '$pluginDir' -Force"
    Invoke-Elevated $command
    Write-Host "已安装：$pluginDir"
    Write-Host "重启 Listary 后生效（退出托盘图标再打开）。"
}
