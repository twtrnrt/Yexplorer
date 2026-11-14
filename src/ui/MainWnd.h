// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/MainWnd.h
//
// 主窗口类。
//
// 当前阶段（阶段 1）职责：
//   1. 创建主窗口、跑消息循环
//   2. 启动后台线程枚举一个目录（先固定为 C:\）
//   3. 枚举完成后把结果画在客户区，同时写进日志
//
// 线程模型（重要）：
//   - UI 线程：只做消息处理与绘制，绝不做磁盘操作
//   - 后台线程：调用 shell::EnumerateFolder 做枚举，只回传纯数据
//   - 线程完成 → PostMessage(WM_APP_ENUM_DONE) → UI 线程重绘
//   - 结果用互斥量保护，避免边读边写
// ============================================================================

#pragma once

#include <windows.h>

#include <mutex>
#include <string>
#include <thread>

#include "core/AsyncTask.h"
#include "shell/ShellFolder.h"

// 主窗口类
class MainWindow
{
public:
    MainWindow();
    ~MainWindow();   // 析构时必须确保后台线程已结束，否则会访问已销毁对象

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    bool Create(HINSTANCE hInstance);
    void Show(int nCmdShow);
    int  RunMessageLoop();

    HWND Handle() const { return m_hWnd; }

private:
    static LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    // ------------------------------------------------------------------
    // 启动后台枚举
    // 会先把上一个还没跑完的任务取消并等待结束，避免任务堆积。
    // ------------------------------------------------------------------
    void StartEnumeration(const std::wstring& path);

    // ------------------------------------------------------------------
    // 后台枚举完成后的处理：写日志 + 请求重绘
    // ------------------------------------------------------------------
    void OnEnumerationDone();

    // ------------------------------------------------------------------
    // 把枚举结果画到客户区（临时展示，阶段 2 会换成真正的文件列表控件）
    // ------------------------------------------------------------------
    void PaintResult(HDC hdc, const RECT& clientRect);

private:
    HWND      m_hWnd      = nullptr;
    HINSTANCE m_hInstance = nullptr;

    // ---- 后台枚举相关状态 ----
    std::thread              m_enumThread;      // 后台枚举线程
    core::CancellationToken  m_enumToken;       // 取消令牌
    std::mutex               m_resultMutex;     // 保护 m_result
    shell::EnumerateResult   m_result;          // 枚举结果（受 m_resultMutex 保护）
    bool                     m_resultReady = false;
    bool                     m_enumRunning = false;
    double                   m_elapsedMs   = 0.0;  // 枚举耗时（毫秒）
    std::wstring             m_enumPath;           // 当前枚举的路径
};
