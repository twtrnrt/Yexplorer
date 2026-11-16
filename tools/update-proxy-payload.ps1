<#
刷新主程序内嵌的代理二进制（res\explorer-proxy-x64.exe）

为什么要跑这个：
  部署进 System32 的 explorer.exe 代理，二进制是焊在主程序里的
  （res\app.rc 的 RCDATA 资源 IDR_EXPLORER_PROXY，ID 定义在 res\resource.h）。
  改了代理源码之后如果不刷新这份 payload，装进 System32 的还是旧代理。

什么时候跑：
  改过 src\launcher\ExplorerLauncher.cpp 或 src\launcher\res\launcher.rc 之后，
  在重新编译主程序之前跑一次。

用法：
  pwsh -File tools\update-proxy-payload.ps1          # 只把现有代理产物复制到 res
  pwsh -File tools\update-proxy-payload.ps1 -Build   # 先编译 x64 代理，再复制

注意：
  主程序是 64 位，所以内嵌的是 x64 版代理（64 位进程启动 explorer.exe 才会命中
  System32，32 位进程命中 SysWOW64，那份默认不做）。
#>
param(
    [switch]$Build
)

$ErrorActionPreference = "Stop"

$root        = Split-Path -Parent $PSScriptRoot
$proxySource = Join-Path $root "bin\launcher\x64\explorer.exe"
$payloadPath = Join-Path $root "res\explorer-proxy-x64.exe"
$msbuild     = "C:\dev\vs2022\MSBuild\Current\Bin\MSBuild.exe"

if ($Build)
{
    Write-Host "编译 x64 代理 …"
    & $msbuild (Join-Path $root "Launcher.vcxproj") /t:Rebuild /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo | Select-Object -Last 2
    if ($LASTEXITCODE -ne 0)
    {
        throw "代理编译失败，退出码 $LASTEXITCODE"
    }
}

if (-not (Test-Path -LiteralPath $proxySource))
{
    throw "找不到代理产物：$proxySource`n先编译 Launcher.vcxproj（或加 -Build 参数），再来跑这个脚本。"
}

Copy-Item -LiteralPath $proxySource -Destination $payloadPath -Force

$payload = Get-Item -LiteralPath $payloadPath
$product = $payload.VersionInfo.ProductName

if ($product -ne "Yexplorer Launcher")
{
    Write-Warning "复制完成，但版本资源里的 ProductName 是「$product」，不是 Yexplorer Launcher。"
    Write-Warning "这样部署时程序认不出它是自己放的代理，会拒绝覆盖已有文件。请检查 launcher.rc。"
}
else
{
    Write-Host "已刷新：$payloadPath"
    Write-Host "  大小 $([math]::Round($payload.Length / 1KB)) KB，ProductName=$product"
}

Write-Host ""
Write-Host "接下来重新编译主程序（Explorer.sln），内嵌的代理才会跟着更新。"
