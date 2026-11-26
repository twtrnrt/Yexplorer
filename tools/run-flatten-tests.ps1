# ============================================================================
# 文件管理器（自研 Explorer 替代品）
# 文件：tools/run-flatten-tests.ps1
#
# 编译并运行第三方菜单树拍平/合并的回归测试（tests/MenuFlattenTest.cpp）。
# 只测纯函数，不启动程序、不读写 %APPDATA%。
# ============================================================================

[CmdletBinding()]
param(
    [string]$VsVcvars = 'C:\dev\vs2022\VC\Auxiliary\Build\vcvars64.bat',
    [string]$BuildDir = ''
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrEmpty($BuildDir)) {
    $BuildDir = Join-Path $env:TEMP 'yexp-flatten-tests'
}
if (-not (Test-Path -LiteralPath $VsVcvars)) {
    throw "找不到 vcvars64.bat：$VsVcvars（可用 -VsVcvars 指定）"
}

New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
$testExe = Join-Path $BuildDir 'MenuFlattenTest.exe'

$cl = @(
    'cl /nologo /std:c++17 /utf-8 /EHsc',
    '/DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00',
    ('/I "{0}\src"' -f $repoRoot),
    ('/Fo"{0}\\"' -f $BuildDir),
    ('"{0}\tests\MenuFlattenTest.cpp" "{0}\src\shell\MenuFlatten.cpp"' -f $repoRoot),
    ('/Fe"{0}"' -f $testExe),
    '/link user32.lib'
) -join ' '

Push-Location $repoRoot
try {
    $compile = 'call "{0}" >nul && {1}' -f $VsVcvars, $cl
    cmd /c $compile
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $testExe)) {
        throw "测试程序编译失败（exit $LASTEXITCODE）"
    }
    & $testExe
    if ($LASTEXITCODE -ne 0) {
        throw "测试未通过（exit $LASTEXITCODE）"
    }
}
finally {
    Pop-Location
}