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
// 【列顺序】按 Windows 资源管理器的默认顺序：
//   名称 / 修改日期 / 类型 / 大小
// ============================================================================

#pragma once

#include <windows.h>
#include <commctrl.h>

#include "core/Format.h"
#include "shell/ShellFolder.h"
#include "shell/ShellIcon.h"

#include <functional>
#include <memory>
#include <string>

class FileListView
{
public:
    using ActivateCallback = std::function<void(size_t index)>;

    FileListView() = default;
    ~FileListView();

    FileListView(const FileListView&) = delete;
    FileListView& operator=(const FileListView&) = delete;

    bool Create(HWND parent, HINSTANCE hInstance);

    // 设置图标缓存（必须在 SetResult 之前设置，否则列表没有图标）
    void SetIconCache(shell::IconCache* iconCache);

    void Resize(int x, int y, int width, int height);

    void SetResult(std::shared_ptr<const shell::EnumerateResult> result);

    void SetActivateCallback(ActivateCallback callback) { m_onActivate = std::move(callback); }

    HWND Handle() const { return m_hWnd; }

    LRESULT HandleNotify(LPARAM lParam);

private:
    void ApplyColumns();
    void LayoutColumns();
    void ApplyFont();

private:
    HWND                                          m_hWnd      = nullptr;
    std::shared_ptr<const shell::EnumerateResult> m_result;
    ActivateCallback                              m_onActivate;
    HFONT                                         m_hFont     = nullptr;

    // 图标缓存不归本控件所有（由主窗口持有并保证生命周期），这里只借指针
    shell::IconCache*                             m_iconCache = nullptr;

    // LVN_GETDISPINFO 回调里生成的文本缓冲（必须比回调活得久，所以放成员）
    std::wstring m_cacheTime;
    std::wstring m_cacheType;
    std::wstring m_cacheSize;
};
