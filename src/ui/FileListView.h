// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/FileListView.h
//
// 文件列表控件（ListView 虚拟模式封装）。
//
// 【为什么用虚拟模式 LVS_OWNERDATA】
//   普通模式要把每一行的数据都塞进控件，一万个文件就意味着控件内部维护
//   一万条记录，打开大目录会明显卡顿。
//   虚拟模式只告诉控件「一共有多少行」，具体每行显示什么，由控件在需要
//   绘制时回调我们（LVN_GETDISPINFO）临时取。这样十万级条目也不卡。
//
// 【数据所有权】
//   列表持有 shared_ptr<const EnumerateResult>，和主窗口共享同一份数据，
//   不做深拷贝。数据不可变，所以多线程读也安全。
// ============================================================================

#pragma once

#include <windows.h>
#include <commctrl.h>

#include "core/Format.h"
#include "shell/ShellFolder.h"

#include <functional>
#include <memory>
#include <string>

// 文件列表控件封装
class FileListView
{
public:
    // 双击某一项时的回调（参数为条目索引）
    using ActivateCallback = std::function<void(size_t index)>;

    FileListView() = default;
    ~FileListView();

    FileListView(const FileListView&) = delete;
    FileListView& operator=(const FileListView&) = delete;

    // ------------------------------------------------------------------
    // 创建控件（作为 parent 的子窗口）
    // ------------------------------------------------------------------
    bool Create(HWND parent, HINSTANCE hInstance);

    // ------------------------------------------------------------------
    // 设置控件在父窗口客户区中的位置和大小
    // 内部会自动重算「名称列自适应宽度」
    // ------------------------------------------------------------------
    void Resize(int x, int y, int width, int height);

    // ------------------------------------------------------------------
    // 设置要显示的数据
    // 传入 shared_ptr，调用方与控件共享同一份不可变数据，零拷贝
    // ------------------------------------------------------------------
    void SetResult(std::shared_ptr<const shell::EnumerateResult> result);

    // 设置双击回调
    void SetActivateCallback(ActivateCallback callback) { m_onActivate = std::move(callback); }

    HWND Handle() const { return m_hWnd; }

    // ------------------------------------------------------------------
    // 处理来自控件的通知消息（由父窗口在 WM_NOTIFY 里转发过来）
    // 返回 0 表示已处理
    // ------------------------------------------------------------------
    LRESULT HandleNotify(LPARAM lParam);

private:
    // 创建四列
    void ApplyColumns();

    // 按当前客户区宽度重算列宽（名称列自适应，其余固定）
    void LayoutColumns();

    // 应用系统 UI 字体
    void ApplyFont();

private:
    HWND                                         m_hWnd   = nullptr;
    std::shared_ptr<const shell::EnumerateResult> m_result;
    ActivateCallback                             m_onActivate;
    HFONT                                        m_hFont  = nullptr;

    // LVN_GETDISPINFO 回调里生成的文本缓冲。
    // 控件会在回调期间读取这些字符串，所以必须放在成员里保证生命周期，
    // 不能用函数内临时变量（否则指针立即悬垂）。
    std::wstring m_cacheSize;
    std::wstring m_cacheType;
    std::wstring m_cacheTime;
};
