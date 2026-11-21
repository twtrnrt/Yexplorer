// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/PasteProgressBar.h
//
// 粘贴进度条：状态栏里的三段式进度显示 + 任务栏按钮上的进度。
//
// 【为什么单独一个模块】
//   这块原来整个塞在 MainWnd.cpp 里（约 460 行）：状态栏子类化、双缓冲自绘、
//   文件边界线、任务栏 ITaskbarList3、进度文字拼装。它跟"窗口在干什么"无关，
//   只跟"粘贴进行到哪"有关，所以收成一个控件类。
//
// 【形状】
//   状态栏切成三格：左文字（正在粘贴：第 3/22 项）/ 进度条 / 右文字（按 ESC 取消）。
//   条本身是固定 300px 的平面色块：已完成深绿、未完成浅灰，再按每个文件的字节
//   占比画 1px 分隔线，蓝条越过哪根线就说明哪个文件传完了。
//
// 【为什么子类化状态栏】
//   系统状态栏只会写一行文字，给不了"两种颜色 + 分隔线"。所以让它照常画完
//   背景和文字后，我们在这块矩形里再补画（WM_PAINT 之后）。
// ============================================================================

#pragma once

#include <windows.h>
#include <shobjidl.h>   // ITaskbarList3

#include <atomic>
#include <memory>
#include <string>
#include <vector>

class PasteProgressBar
{
public:
    PasteProgressBar() = default;
    ~PasteProgressBar();

    PasteProgressBar(const PasteProgressBar&) = delete;
    PasteProgressBar& operator=(const PasteProgressBar&) = delete;

    // ownerWindow：任务栏进度挂在哪；statusBar：要子类化的状态栏
    void Attach(HWND ownerWindow, HWND statusBar);
    void Detach();

    // 一批粘贴开始：清单每项字节数（可为空）、已写字节（后台线程累加）、总字节、总项数
    void Begin(const std::shared_ptr<std::vector<LONGLONG>>& itemSizes,
               const std::shared_ptr<std::atomic<LONGLONG>>& doneBytes,
               LONGLONG totalBytes,
               size_t   totalItems);

    // 清单到达 / 总量变化：只更新数字并重排（项数按清单长度算）
    void SetPlan(const std::shared_ptr<std::vector<LONGLONG>>& itemSizes,
                 const std::shared_ptr<std::atomic<LONGLONG>>& doneBytes,
                 LONGLONG totalBytes);

    // 进度更新：index 从 1 开始；percent = -1 表示来源没给大小（只知道在做）
    void Update(size_t index, size_t totalItems, int percent);

    // 占位文案（"正在粘贴…" / "正在取消粘贴…"）
    void SetText(const std::wstring& text);

    // 把当前文字重新写进第 0 格（状态栏被别处改过时恢复；粘贴期间 UpdateStatusBar 会调）
    void RefreshText();


    // 状态栏尺寸变化时重排
    void Layout();

    // 结束：状态栏恢复一整格、任务栏进度撤掉
    void End();

private:
    // 状态栏子类化：系统画完之后补画进度条
    static LRESULT CALLBACK StatusBarSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
                                                  LPARAM lParam, UINT_PTR idSubclass,
                                                  DWORD_PTR refData);

    void InitTaskbarProgress();
    void Paint();
    void BuildLines(int barWidth);
    int  Scale(int value) const;

private:
    HWND          m_ownerWindow = nullptr;
    HWND          m_statusBar   = nullptr;
    ITaskbarList3* m_taskbar    = nullptr;

    // 数据来源（共享指针：后台线程写，这里只读）
    std::shared_ptr<std::vector<LONGLONG>> m_itemSizes;
    std::shared_ptr<std::atomic<LONGLONG>> m_doneBytes;
    LONGLONG      m_totalBytes = 0;
    size_t        m_totalItems = 0;

    // 显示状态
    int           m_position   = 0;          // 0..1000
    RECT          m_barRect    = {};         // 条在状态栏客户区里的矩形
    std::vector<int> m_lineOffsets;          // 文件边界线的条内 x 偏移（已去重）
    bool          m_linesDirty = true;
    std::wstring  m_progressText;            // 当前第 0 格文字（没变就不重写）
};
