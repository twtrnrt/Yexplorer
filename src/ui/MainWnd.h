// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/MainWnd.h
//
// 主窗口类（阶段 2 版本）。
//
// 结构：
//   主窗口
//     └─ FileListView（虚拟 ListView，铺满客户区）
//
// 线程模型（与阶段 1 相同）：
//   - UI 线程只做消息处理与布局
//   - 后台线程做目录枚举，回传纯数据
//   - 结果通过 PostMessage 通知 UI，再用 shared_ptr 交给列表控件
// ============================================================================

#pragma once

#include <windows.h>

#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/AsyncTask.h"
#include "shell/ShellFolder.h"
#include "ui/FileListView.h"

class MainWindow
{
public:
    MainWindow();
    ~MainWindow();

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    bool Create(HINSTANCE hInstance);
    void Show(int nCmdShow);
    int  RunMessageLoop();

    HWND Handle() const { return m_hWnd; }

private:
    static LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    // 启动后台枚举（会先取消上一个任务）
    void StartEnumeration(const std::wstring& path);

    // 后台枚举完成：排序 → 交给列表 → 写日志 → 更新标题
    void OnEnumerationDone();

    // 双击某一项：文件夹就进入，文件就调用系统默认程序打开
    void OnItemActivated(size_t index);

    // 按客户区大小摆放子控件
    void LayoutChildren();

private:
    HWND         m_hWnd      = nullptr;
    HINSTANCE    m_hInstance = nullptr;
    FileListView m_fileListView;

    // ---- 后台枚举状态 ----
    std::thread                                  m_enumThread;
    core::CancellationToken                      m_enumToken;
    std::mutex                                   m_resultMutex;
    std::shared_ptr<shell::EnumerateResult>      m_result;
    bool                                         m_resultReady = false;
    double                                       m_elapsedMs   = 0.0;
    std::wstring                                 m_enumPath;
};
