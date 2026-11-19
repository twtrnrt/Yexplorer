<#
============================================================================
 tools/check-version.ps1 -- 构建后版本号自检

 为什么要有这个脚本：
   一个 VERSIONINFO 资源里有两份互相独立的版本号：
     · FILEVERSION / PRODUCTVERSION  -> 编进二进制的 VS_FIXEDFILEINFO
     · "FileVersion" / "ProductVersion" 字符串 -> 存在资源字符串表里
   rc.exe 不会检查两者是否一致。Windows 的安装器、更新器、脚本读的是
   二进制那份，资源管理器「属性 -> 详细信息」显示的是字符串那份，于是会
   出现"看着是 1.6、实际是 1.5"的情况（本项目已经踩过两次）。

 这个脚本做两件事：
   1. 检查 res\version.h 里的四个数字和字符串本身是否一致；
   2. 检查编译出来的 exe 里，二进制固定值和字符串是否都等于 version.h。

 用法：
   powershell -NoProfile -File tools\check-version.ps1
   powershell -NoProfile -File tools\check-version.ps1 -Exe bin\x64\Release\FileExplorer.exe
   powershell -NoProfile -File tools\check-version.ps1 -RequireAll   # 缺文件也算失败

 退出码：0 = 全部一致；1 = 有不一致；2 = version.h 读不出来。
============================================================================
#>
[CmdletBinding()]
param(
    # 要检查的 exe。不传就按默认三处：Release、Debug、dist\<版本>\FileExplorer.exe
    [string[]]$Exe,
    # 缺文件时是否算失败（默认只警告，因为 Debug 可能还没编）
    [switch]$RequireAll
)

$ErrorActionPreference = 'Stop'

$root          = Split-Path -Parent $PSScriptRoot
$versionHeader = Join-Path $root 'res\version.h'

if (-not (Test-Path -LiteralPath $versionHeader)) {
    Write-Host "找不到版本头文件：$versionHeader" -ForegroundColor Red
    exit 2
}

$text = Get-Content -LiteralPath $versionHeader -Raw -Encoding UTF8

function Read-DefineInt([string]$name) {
    $m = [regex]::Match($text, "(?m)^\s*#define\s+$name\s+(\d+)\s*$")
    if (-not $m.Success) {
        Write-Host "res\version.h 里找不到 #define $name" -ForegroundColor Red
        exit 2
    }
    return [int]$m.Groups[1].Value
}

$major = Read-DefineInt 'YEXP_VER_MAJOR'
$minor = Read-DefineInt 'YEXP_VER_MINOR'
$patch = Read-DefineInt 'YEXP_VER_PATCH'
$build = Read-DefineInt 'YEXP_VER_BUILD'

$mString = [regex]::Match($text, '(?m)^\s*#define\s+YEXP_VER_STRING\s+"([^"]+)"\s*$')
if (-not $mString.Success) {
    Write-Host 'res\version.h 里找不到 #define YEXP_VER_STRING "x.y.z.w"' -ForegroundColor Red
    exit 2
}
$declared = $mString.Groups[1].Value
$expected = '{0}.{1}.{2}.{3}' -f $major, $minor, $patch, $build

Write-Host ''
Write-Host ('版本来源：res\version.h  ->  {0}  (数字 {1},{2},{3},{4})' -f $expected, $major, $minor, $patch, $build)

$fail = 0

if ($declared -ne $expected) {
    Write-Host ("  [失败] 字符串写的是 {0}，和四个数字推出的 {1} 不一致" -f $declared, $expected) -ForegroundColor Red
    $fail++
}

# ---------------------------------------------------------------- 检查 exe
if (-not $Exe -or $Exe.Count -eq 0) {
    $distName = '{0}.{1}.{2}' -f $major, $minor, $patch
    $Exe = @(
        (Join-Path $root 'bin\x64\Release\FileExplorer.exe'),
        (Join-Path $root 'bin\x64\Debug\FileExplorer.exe'),
        (Join-Path $root ('dist\{0}\FileExplorer.exe' -f $distName))
    )
}

foreach ($path in $Exe) {
    $full = if ([System.IO.Path]::IsPathRooted($path)) { $path } else { Join-Path $root $path }
    $shown = $full.Replace($root + '\', '')

    if (-not (Test-Path -LiteralPath $full)) {
        if ($RequireAll) {
            Write-Host ("  [失败] {0} 不存在" -f $shown) -ForegroundColor Red
            $fail++
        } else {
            Write-Host ("  [跳过] {0} 不存在（还没编）" -f $shown) -ForegroundColor DarkYellow
        }
        continue
    }

    $info      = (Get-Item -LiteralPath $full).VersionInfo
    $fixedFile = "$($info.FileVersionRaw)"      # VS_FIXEDFILEINFO
    $fixedProd = "$($info.ProductVersionRaw)"
    $strFile   = "$($info.FileVersion)"         # StringFileInfo
    $strProd   = "$($info.ProductVersion)"

    $ok = ($fixedFile -eq $expected) -and ($fixedProd -eq $expected) -and
          ($strFile -eq $expected) -and ($strProd -eq $expected)

    if ($ok) {
        Write-Host ("  [通过] {0}  固定={1} 字符串={2}" -f $shown, $fixedFile, $strFile) -ForegroundColor Green
    } else {
        Write-Host ("  [失败] {0}" -f $shown) -ForegroundColor Red
        Write-Host ("         期望 {0}；实际 固定文件={1} 固定产品={2} 字符串文件={3} 字符串产品={4}" `
                    -f $expected, $fixedFile, $fixedProd, $strFile, $strProd) -ForegroundColor Red
        $fail++
    }
}

Write-Host ''
if ($fail -gt 0) {
    Write-Host ("版本号自检失败：{0} 处不一致" -f $fail) -ForegroundColor Red
    exit 1
}

Write-Host '版本号自检通过（version.h、二进制固定值、字符串三者一致）' -ForegroundColor Green
exit 0
