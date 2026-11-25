# ============================================================================
# 文件管理器（自研 Explorer 替代品）
# 文件：tools/run-menu-layout-tests.ps1
#
# 编译并运行「自定义右键菜单」布局的回归测试（tests/MenuLayoutTest.cpp）。
#
# 只测纯函数：默认布局、顺序解析、分隔线折叠、JSON 读写与坏配置回退。
# 不启动程序、不读写 %APPDATA% 里的真实配置。
#
# 用法（仓库根目录）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\run-menu-layout-tests.ps1
#   退出码 0 = 全部通过。
# ============================================================================

[CmdletBinding()]
param(
    [string]$VsVcvars = 'C:\dev\vs2022\VC\Auxiliary\Build\vcvars64.bat',
    [string]$BuildDir = ''
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrEmpty($BuildDir)) {
    $BuildDir = Join-Path $env:TEMP 'yexp-menu-tests'
}

if (-not (Test-Path -LiteralPath $VsVcvars)) {
    throw "找不到 vcvars64.bat：$VsVcvars（可用 -VsVcvars 指定）"
}

New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null

$testExe = Join-Path $BuildDir 'MenuLayoutTest.exe'

$cl = @(
    'cl /nologo /std:c++17 /utf-8 /EHsc',
    '/DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00',
    ('/I "{0}\src" /I "{0}\third" /I "{0}\third\spdlog\include"' -f $repoRoot),
    ('/Fo"{0}\\"' -f $BuildDir),
    ('"{0}\tests\MenuLayoutTest.cpp" "{0}\src\ui\MenuLayout.cpp" "{0}\src\core\Logger.cpp"' -f $repoRoot),
    ('/Fe"{0}"' -f $testExe),
    '/link shell32.lib ole32.lib user32.lib advapi32.lib'
) -join ' '

Push-Location $repoRoot
try {
    # vcvars 是批处理，必须经 cmd 调；>nul 吞掉版本横幅
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
