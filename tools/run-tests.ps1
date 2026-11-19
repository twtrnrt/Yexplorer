# ============================================================================
# 文件管理器（自研 Explorer 替代品）
# 文件：tools/run-tests.ps1
#
# 编译并运行批量重命名规则引擎的回归测试（tests/BatchRenameRulesTest.cpp）。
#
# 测试读 tests/data/batch-rename-baseline.csv 作为行为基准 —— 那份 CSV 是
# C8-1 重构【之前】的实现跑出来的；以后改规则引擎时，只要这份测试还是全绿，
# 就说明结果和旧行为逐字一致（要故意改行为的话，先改基准并在提交信息里说明）。
#
# 用法（仓库根目录）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\run-tests.ps1
#   退出码 0 = 全部通过。
#
# 说明：测试程序单独用 cl.exe 编译（不经过 Explorer.vcxproj），
#       中间文件放在 %TEMP%\yexp-tests，不污染仓库。
# ============================================================================

[CmdletBinding()]
param(
    [string]$VsVcvars = 'C:\dev\vs2022\VC\Auxiliary\Build\vcvars64.bat',
    [string]$BuildDir = ''
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrEmpty($BuildDir)) {
    $BuildDir = Join-Path $env:TEMP 'yexp-tests'
}

if (-not (Test-Path -LiteralPath $VsVcvars)) {
    throw "找不到 vcvars64.bat：$VsVcvars（可用 -VsVcvars 指定）"
}

New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null

$testExe = Join-Path $BuildDir 'BatchRenameRulesTest.exe'
$baseline = Join-Path $repoRoot 'tests\data\batch-rename-baseline.csv'

$cl = @(
    'cl /nologo /std:c++17 /utf-8 /EHsc',
    '/DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00',
    ('/I "{0}\src" /I "{0}\tests"' -f $repoRoot),
    ('/Fo"{0}\\"' -f $BuildDir),
    ('"{0}\tests\BatchRenameRulesTest.cpp" "{0}\src\ui\BatchRenameDialog.cpp"' -f $repoRoot),
    ('/Fe"{0}"' -f $testExe),
    '/link user32.lib gdi32.lib comctl32.lib uxtheme.lib'
) -join ' '

Push-Location $repoRoot
try {
    # 必须用 cmd 调 vcvars（它是个批处理）；>nul 把 vcvars 的版本横幅吞掉
    $compile = 'call "{0}" >nul && {1}' -f $VsVcvars, $cl
    cmd /c $compile

    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $testExe)) {
        throw "测试程序编译失败（exit $LASTEXITCODE）"
    }

    & $testExe $baseline
    $result = $LASTEXITCODE

    if ($result -ne 0) {
        throw "测试失败（exit $result）"
    }
}
finally {
    Pop-Location
}

Write-Host '测试通过' -ForegroundColor Green
exit 0
