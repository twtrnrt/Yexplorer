// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/MainWnd.h
//
// 主窗口类：负责注册窗口类、创建主窗口、处理窗口消息、驱动消息循环。
//
// 设计说明：
//   1. 采用 C++ 类的形式封装，但不做消息映射宏（不引入 WTL 等框架），
//      所有消息在一处 switch 里处理，逻辑直白、便于排查。
//   2. 窗口过程是静态函数，通过 GWLP_USERDATA 把 this 指针挂到窗口上，
//      再转发给成员函数——这是 Win32 + C++ 的常规做法。
// ============================================================================

#pragma once

#include <windows.h>

// 主窗口类
class MainWindow
{
public:
    MainWindow() = default;
    ~MainWindow() = default;

    // 禁止拷贝：窗口对象持有 HWND，拷贝没有意义且容易造成句柄重复管理
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    // ------------------------------------------------------------------
    // 创建主窗口
    // 参数：hInstance —— 当前进程实例句柄（wWinMain 的第一个参数）
    // 返回：成功 true，失败 false
    // ------------------------------------------------------------------
    bool Create(HINSTANCE hInstance);

    // ------------------------------------------------------------------
    // 显示主窗口
    // 参数：nCmdShow —— 显示方式（SW_SHOW / SW_SHOWMAXIMIZED 等）
    // ------------------------------------------------------------------
    void Show(int nCmdShow);

    // ------------------------------------------------------------------
    // 进入消息循环
    // 返回：进程退出码（WM_QUIT 携带的 wParam）
    // ------------------------------------------------------------------
    int RunMessageLoop();

    // 获取主窗口句柄（子控件创建时需要用父窗口句柄）
    HWND Handle() const { return m_hWnd; }

private:
    // ------------------------------------------------------------------
    // 静态窗口过程：
    // 窗口创建时（WM_NCCREATE）从 CREATESTRUCT 里取出 this，挂到 GWLP_USERDATA，
    // 之后所有消息都转发给 HandleMessage。
    // ------------------------------------------------------------------
    static LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // ------------------------------------------------------------------
    // 成员消息处理：真正处理窗口消息的地方
    // ------------------------------------------------------------------
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

private:
    HWND      m_hWnd      = nullptr;  // 主窗口句柄
    HINSTANCE m_hInstance = nullptr;  // 进程实例句柄
};
