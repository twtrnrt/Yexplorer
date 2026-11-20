// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/ShellWindowHost.h
//
// 把主窗口注册成一个"Shell 窗口"（IShellWindows）。
//
// 【为什么需要】
//   有些程序（Chrome/Edge 下载、"打开文件位置"、Listary 快速切换）不是把
//   "要选中的文件"写在命令行里，而是：
//     1. 用 IShellWindows 找到"正在浏览这个目录"的窗口；
//     2. 再发一条 WM_COPYDATA（0xE32EE32E）把选中项的 PIDL 塞给那个窗口。
//   不注册的话，这些程序只看到系统资源管理器，我们既不会被选中也读不到目录。
//
// 【安全边界】
//   - 只在设置开关打开时才注册；关闭 = 完全不注册，行为和以前一样；
//   - 只注册主窗口，对话框不注册；
//   - 只暴露"当前目录 / 窗口句柄 / 名称"这类只读信息，Document（窗口内容
//     自动化）一律返回 E_NOTIMPL，不假装支持；
//   - 不写注册表，纯运行时注册，Revoke 后不留痕迹。
// ============================================================================

#pragma once

#include <windows.h>
#include <objbase.h>
#include <oaidl.h>
#include <exdisp.h>

#include <string>

#include <wil/com.h>

namespace ui
{

// 自动化对象调回主窗口用的两条消息（地址栏导航 / 刷新）
constexpr UINT kShellWindowNavigateMessage = WM_APP + 0x120;
constexpr UINT kShellWindowRefreshMessage  = WM_APP + 0x121;

class ShellWindowHost
{
public:
    ShellWindowHost();
    ~ShellWindowHost();

    ShellWindowHost(const ShellWindowHost&) = delete;
    ShellWindowHost& operator=(const ShellWindowHost&) = delete;

    // 主窗口建好后注册；folder 是当前目录（还没枚举完可以传空，之后 UpdateFolder）
    bool Register(HWND window, const std::wstring& folder);

    // 每次导航/枚举完成后调用，外部程序读到的就是这里
    void UpdateFolder(const std::wstring& folder);

    // 窗口销毁 / 关掉开关时调用
    void Revoke();

    bool IsRegistered() const { return m_cookie != 0; }

    // 给自动化对象用的只读访问
    HWND Window() const { return m_window; }
    std::wstring Folder() const { return m_folder; }
    static std::wstring OwnExePath();

private:
    class Automation;
    friend class Automation;

    wil::com_ptr<IShellWindows> m_shellWindows;
    wil::com_ptr<IDispatch>     m_automation;
    long                        m_cookie = 0;
    HWND                        m_window = nullptr;
    std::wstring                m_folder;
};

} // namespace ui
