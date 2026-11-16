// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：res/resource.h —— 资源 ID 定义
//
// 这个头文件同时被 res/app.rc（资源脚本）和 C++ 代码引用，
// 保证两边用的是同一个数字，不会对不上。
//
// 注意：本文件保持纯 ASCII，不写中文，避免 rc.exe 的编码问题。
// ============================================================================

#define IDI_APPICON 101   // 程序图标（exe 图标 + 主窗口标题栏 / 任务栏图标）

// Binary of the explorer.exe thin proxy, embedded as an RCDATA resource and
// released when deploying it into System32. This keeps the release package to a
// single FileExplorer.exe (no extra explorer-proxy.exe file).
// Payload file: res\explorer-proxy-x64.exe, refreshed from
// bin\launcher\x64\explorer.exe by tools\update-proxy-payload.ps1.
#define IDR_EXPLORER_PROXY 201
