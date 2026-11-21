// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/PasteProgressBar.cpp
//
// 状态栏粘贴进度条 + 任务栏进度。说明见 PasteProgressBar.h。
// ============================================================================

#include "PasteProgressBar.h"

#include "core/Logger.h"

PasteProgressBar::~PasteProgressBar()
{
    Detach();
}

// ============================================================================
// 挂接 / 摘除
// ============================================================================

void PasteProgressBar::Attach(HWND ownerWindow, HWND statusBar)
{
    Detach();

    m_ownerWindow = ownerWindow;
    m_statusBar   = statusBar;

    if (m_statusBar == nullptr)
    {
        return;
    }

    // 系统状态栏只会写一行文字，画不出"两种颜色 + 分隔线"，
    // 所以让它照常画完背景和文字后，再在那块矩形里补画。
    ::SetWindowSubclass(m_statusBar, &PasteProgressBar::StatusBarSubclassProc, 1,
                        reinterpret_cast<DWORD_PTR>(this));

    InitTaskbarProgress();
}

void PasteProgressBar::Detach()
{
    if (m_statusBar != nullptr)
    {
        ::RemoveWindowSubclass(m_statusBar, &PasteProgressBar::StatusBarSubclassProc, 1);
    }

    if (m_taskbar != nullptr)
    {
        m_taskbar->Release();
        m_taskbar = nullptr;
    }

    m_statusBar   = nullptr;
    m_ownerWindow = nullptr;
}

// ----------------------------------------------------------------------------
// 任务栏按钮上的进度条（ITaskbarList3）
//
// 用途：粘贴大文件时用户可能切到别的程序去干别的，任务栏按钮上那条进度能一眼
// 看到还剩多少。颜色/粗细由系统定（正常=主题绿、暂停=黄、错误=红），改不了。
//
// 失败：精简系统、某些远程/服务会话里可能拿不到这个对象或 HrInit 失败，
// 那就保持 nullptr，所有调用点判空跳过 —— 状态栏那条照旧工作，不影响功能。
// ----------------------------------------------------------------------------
void PasteProgressBar::InitTaskbarProgress()
{
    if (m_taskbar != nullptr || m_ownerWindow == nullptr)
    {
        return;
    }

    ITaskbarList3* list = nullptr;

    const HRESULT createHr = ::CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                                IID_PPV_ARGS(&list));

    if (FAILED(createHr) || list == nullptr)
    {
        Logger::Warn(L"任务栏进度不可用（创建失败）：" + std::to_wstring(static_cast<long long>(createHr)));
        return;
    }

    const HRESULT initHr = list->HrInit();

    if (FAILED(initHr))
    {
        Logger::Warn(L"任务栏进度不可用（HrInit 失败）：" + std::to_wstring(static_cast<long long>(initHr)));
        list->Release();
        return;
    }

    m_taskbar = list;
    Logger::Info(L"任务栏进度已就绪");
}

// ============================================================================
// 开始 / 更新 / 结束
// ============================================================================

void PasteProgressBar::Begin(const std::shared_ptr<std::vector<LONGLONG>>& itemSizes,
                             const std::shared_ptr<std::atomic<LONGLONG>>& doneBytes,
                             LONGLONG totalBytes,
                             size_t   totalItems)
{
    m_itemSizes  = itemSizes;
    m_doneBytes  = doneBytes;
    m_totalBytes = totalBytes;
    m_totalItems = totalItems;
    m_position   = 0;
    m_linesDirty = true;

    m_progressText = L"正在粘贴…";

    // 状态栏切成"左文字 / 条 / 右文字"三格
    Layout();

    if (m_statusBar != nullptr)
    {
        ::SendMessageW(m_statusBar, SB_SETTEXTW, 0,
                       reinterpret_cast<LPARAM>(m_progressText.c_str()));
    }

    // 任务栏按钮上也挂一条进度（切到别的程序也能看到剩多少）
    if (m_taskbar != nullptr && m_ownerWindow != nullptr)
    {
        m_taskbar->SetProgressState(m_ownerWindow, TBPF_NORMAL);
        m_taskbar->SetProgressValue(m_ownerWindow, 0, 1000);
    }
}

void PasteProgressBar::SetPlan(const std::shared_ptr<std::vector<LONGLONG>>& itemSizes,
                               const std::shared_ptr<std::atomic<LONGLONG>>& doneBytes,
                               LONGLONG totalBytes)
{
    m_itemSizes  = itemSizes;
    m_doneBytes  = doneBytes;
    m_totalBytes = totalBytes;

    if (m_itemSizes != nullptr && !m_itemSizes->empty())
    {
        m_totalItems = m_itemSizes->size();
    }

    // 清单到了（项数已知）：按"第 N/N 项"这种最长文字重排一次，
    // 之后进度条位置就固定不动了（进度更新只改文字和填色）。
    m_linesDirty = true;
    Layout();
}

void PasteProgressBar::SetText(const std::wstring& text)
{
    m_progressText = text;

    RefreshText();
}

void PasteProgressBar::RefreshText()
{
    if (m_statusBar == nullptr || m_progressText.empty())
    {
        return;
    }

    ::SendMessageW(m_statusBar, SB_SETTEXTW, 0,
                   reinterpret_cast<LPARAM>(m_progressText.c_str()));
}

void PasteProgressBar::End()
{
    if (m_statusBar != nullptr)
    {
        int parts[1] = { -1 };
        ::SendMessageW(m_statusBar, SB_SETPARTS, 1, reinterpret_cast<LPARAM>(parts));
        ::SendMessageW(m_statusBar, SB_SETTEXTW, 2, 0);
    }

    m_position   = 0;
    m_linesDirty = true;
    m_lineOffsets.clear();
    ::SetRectEmpty(&m_barRect);

    m_progressText.clear();
    m_itemSizes.reset();
    m_doneBytes.reset();
    m_totalBytes = 0;
    m_totalItems = 0;

    if (m_taskbar != nullptr && m_ownerWindow != nullptr)
    {
        m_taskbar->SetProgressState(m_ownerWindow, TBPF_NOPROGRESS);
    }
}

// ============================================================================
// 布局
// ============================================================================

void PasteProgressBar::Layout()
{
    if (m_statusBar == nullptr)
    {
        return;
    }

    RECT client = {};
    ::GetClientRect(m_statusBar, &client);

    const int totalWidth  = client.right - client.left;
    const int totalHeight = client.bottom - client.top;

    // 按"这一批里会出现的最长文字"算一次宽度，之后就不再动进度条的位置。
    //   项数已知（清单到了）→ 用真项数，比如 22 项就按"第 22/22 项"量；
    //   项数还不知道（刚开始）→ 先按"正在粘贴"占位，清单到了会再算一次。
    const size_t totalItems = (m_itemSizes != nullptr && !m_itemSizes->empty())
                              ? m_itemSizes->size()
                              : m_totalItems;

    std::wstring longestLeftText = L"正在粘贴";

    if (totalItems > 0)
    {
        const std::wstring count = std::to_wstring(totalItems);
        longestLeftText = L"正在粘贴：第 " + count + L"/" + count + L" 项";
    }

    // 右格：取消方式 + 传输期间的提醒（固定文案，不随进度变化）
    const std::wstring rightText = L"（按 ESC 取消）　传输完成前不要使用复制/剪切功能";

    HDC   dc   = ::GetDC(m_statusBar);
    HFONT font = reinterpret_cast<HFONT>(::SendMessageW(m_statusBar, WM_GETFONT, 0, 0));
    HGDIOBJ old = ::SelectObject(dc, font != nullptr ? font : ::GetStockObject(DEFAULT_GUI_FONT));

    SIZE leftSize  = {};
    SIZE rightSize = {};
    ::GetTextExtentPoint32W(dc, longestLeftText.c_str(), static_cast<int>(longestLeftText.size()), &leftSize);
    ::GetTextExtentPoint32W(dc, rightText.c_str(),       static_cast<int>(rightText.size()),       &rightSize);

    ::SelectObject(dc, old);
    ::ReleaseDC(m_statusBar, dc);

    // 左文字格：字宽 + 12px。条自己在左端还内缩 3px（见 Paint 的 barLeft），
    // 所以这里减掉 3，保证"文字末 → 条的左边缘"实测就是 12px。
    int leftWidth  = leftSize.cx + Scale(12) - Scale(3);
    int rightWidth = rightSize.cx + Scale(10);

    // 进度条固定 300px（不是拉满整条状态栏），右边留白无所谓
    int barWidth = Scale(300);

    const int available = totalWidth - leftWidth - rightWidth;

    if (barWidth > available)
    {
        // 窗口太窄时缩到可用宽度，但至少留一点，别画成一条线
        barWidth = (available > Scale(80)) ? available : Scale(80);
    }

    int parts[3] = { leftWidth, leftWidth + barWidth, -1 };
    ::SendMessageW(m_statusBar, SB_SETPARTS, 3, reinterpret_cast<LPARAM>(parts));

    // 第 1 格留空（不写文字），它的位置记下来，补画时用
    ::SendMessageW(m_statusBar, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(L""));

    // 右格文字固定，不随进度变化
    ::SendMessageW(m_statusBar, SB_SETTEXTW, 2, reinterpret_cast<LPARAM>(rightText.c_str()));

    m_barRect.left   = leftWidth;
    m_barRect.top    = 0;
    m_barRect.right  = leftWidth + barWidth;
    m_barRect.bottom = totalHeight;

    // 条的位置/宽度变了，分隔线的像素位置要重算
    m_linesDirty = true;
}

int PasteProgressBar::Scale(int value) const
{
    UINT dpi = 96;

    if (m_statusBar != nullptr)
    {
        dpi = ::GetDpiForWindow(m_statusBar);
        if (dpi == 0)
        {
            dpi = 96;
        }
    }

    return ::MulDiv(value, static_cast<int>(dpi), 96);
}

// ============================================================================
// 绘制
// ============================================================================

void PasteProgressBar::Paint()
{
    if (m_statusBar == nullptr || ::IsRectEmpty(&m_barRect))
    {
        return;
    }

    RECT rc = m_barRect;

    // 条本身 15px 高（状态栏装不下时自动缩），上下居中，两端各留一点空
    const int statusHeight = rc.bottom - rc.top;

    int barHeight = Scale(15);

    if (barHeight > statusHeight - Scale(4))
    {
        barHeight = statusHeight - Scale(4);
    }

    if (barHeight < Scale(6))
    {
        barHeight = Scale(6);
    }

    const int top = rc.top + (statusHeight - barHeight) / 2;

    const int barLeft  = rc.left + Scale(3);
    const int barRight = rc.right - Scale(3);

    if (barRight <= barLeft)
    {
        return;
    }

    const int barWidth = barRight - barLeft;

    // 线的位置补算（只在清单到达/条宽变化后做一次）
    if (m_linesDirty)
    {
        BuildLines(barWidth);
        m_linesDirty = false;
    }

    // ------------------------------------------------------------------
    // 双缓冲：先在内存位图里把整条画完，再一次性 BitBlt 到状态栏
    //
    // 为什么不直接画在状态栏上：直接画是"铺灰底 → 铺绿 → 一根根画线"，
    // 三个步骤在屏幕上逐步发生，文件多、线条多的时候就被看成闪。
    // 画到内存里再整块贴上去，屏幕上只出现最终结果，不会闪。
    // ------------------------------------------------------------------
    HDC screenDc = ::GetDC(m_statusBar);

    if (screenDc == nullptr)
    {
        return;
    }

    HDC     memDc  = ::CreateCompatibleDC(screenDc);
    HBITMAP bitmap = ::CreateCompatibleBitmap(screenDc, barWidth, barHeight);

    if (memDc == nullptr || bitmap == nullptr)
    {
        if (memDc != nullptr)
        {
            ::DeleteDC(memDc);
        }

        if (bitmap != nullptr)
        {
            ::DeleteObject(bitmap);
        }

        ::ReleaseDC(m_statusBar, screenDc);
        return;
    }

    HGDIOBJ oldBitmap = ::SelectObject(memDc, bitmap);

    // 内存里以 (0,0) 作为条的左上角，所以下面所有坐标都是"条内坐标"
    RECT track = { 0, 0, barWidth, barHeight };

    // 1. 未完成部分：浅灰底（214,214,214）
    HBRUSH undoneBrush = ::CreateSolidBrush(RGB(214, 214, 214));
    ::FillRect(memDc, &track, undoneBrush);
    ::DeleteObject(undoneBrush);

    // 2. 已完成部分：15,123,15（绿），长度按进度
    int filledWidth = static_cast<int>(
        static_cast<double>(barWidth) * m_position / 1000.0 + 0.5);

    if (filledWidth > barWidth)
    {
        filledWidth = barWidth;
    }

    if (filledWidth > 0)
    {
        RECT doneRect = { 0, 0, filledWidth, barHeight };
        HBRUSH doneBrush = ::CreateSolidBrush(RGB(15, 123, 15));
        ::FillRect(memDc, &doneRect, doneBrush);
        ::DeleteObject(doneBrush);
    }

    // 3. 文件边界线：每根 1px；已经走到的用绿色，没走到的用灰 150
    if (!m_lineOffsets.empty())
    {
        HBRUSH pendingLineBrush = ::CreateSolidBrush(RGB(150, 150, 150));
        HBRUSH passedLineBrush  = ::CreateSolidBrush(RGB(15, 123, 15));

        for (int offset : m_lineOffsets)
        {
            if (offset <= 0 || offset >= barWidth)
            {
                continue;   // 双保险：别画到条的边框上
            }

            HBRUSH brush = (offset <= filledWidth) ? passedLineBrush : pendingLineBrush;

            RECT line = { offset, 0, offset + 1, barHeight };
            ::FillRect(memDc, &line, brush);
        }

        ::DeleteObject(pendingLineBrush);
        ::DeleteObject(passedLineBrush);
    }

    // 4. 一次贴到状态栏上
    ::BitBlt(screenDc, barLeft, top, barWidth, barHeight, memDc, 0, 0, SRCCOPY);

    ::SelectObject(memDc, oldBitmap);
    ::DeleteObject(bitmap);
    ::DeleteDC(memDc);
    ::ReleaseDC(m_statusBar, screenDc);
}

// ----------------------------------------------------------------------------
// 算一次"文件分隔线"的像素位置（存成条内偏移，已去重）
//
// 为什么只算一次、还要去重：
//   文件几百上千个的时候，绝大多数线的 x 会落在同一像素列上（条才 300px 宽）。
//   原来每帧都按文件数逐根重算、逐根 FillRect，等于把同一根线重复画几百次：
//   既浪费，又让这一格的重绘变慢，用户看到的就是"进度条在闪"。
//   现在按像素去重后缓存，每帧最多只画"条宽"那么多根，开销跟文件数无关。
// ----------------------------------------------------------------------------
void PasteProgressBar::BuildLines(int barWidth)
{
    m_lineOffsets.clear();

    if (m_itemSizes == nullptr || m_totalBytes <= 0 || barWidth <= 0)
    {
        return;
    }

    const size_t count = m_itemSizes->size();

    if (count < 2)
    {
        return;   // 只有一项，没有"文件之间"的边界
    }

    LONGLONG cumulative = 0;
    int      lastOffset = -1;

    for (size_t i = 0; i + 1 < count; ++i)
    {
        cumulative += (*m_itemSizes)[i];

        const int offset = static_cast<int>(
            static_cast<double>(barWidth) * static_cast<double>(cumulative) /
            static_cast<double>(m_totalBytes) + 0.5);

        if (offset <= 0 || offset >= barWidth)
        {
            continue;   // 贴着条的两端，不画
        }

        if (offset == lastOffset)
        {
            continue;   // 和上一根落在同一像素 → 只留一根（就是"重叠的线合并"）
        }

        m_lineOffsets.push_back(offset);
        lastOffset = offset;
    }
}

// ============================================================================
// 进度更新
// ============================================================================

void PasteProgressBar::Update(size_t index, size_t totalItems, int percent)
{
    if (m_statusBar == nullptr)
    {
        return;
    }

    if (totalItems > 0)
    {
        m_totalItems = totalItems;
    }

    // ------------------------------------------------------------------
    // 算整批的完成度（0..1）
    //
    // 按字节算：一格代表差不多的字节量，于是大文件占的格子多、走得慢，
    // 小文件占的格子少、走得快（也就是"一个文件一格，文件越大格子越宽"）。
    // 清单里一个大小都没给的时候，退回按项数估。
    // ------------------------------------------------------------------
    double progress = 0.0;

    const LONGLONG totalBytes = m_totalBytes;
    const LONGLONG doneBytes  = (m_doneBytes != nullptr) ? m_doneBytes->load() : 0;

    LONGLONG currentBytes = 0;
    if (m_itemSizes != nullptr && index >= 1 && index <= m_itemSizes->size())
    {
        currentBytes = (*m_itemSizes)[index - 1];
    }

    if (totalBytes > 0)
    {
        LONGLONG written = doneBytes;

        if (percent > 0 && currentBytes > 0)
        {
            written += currentBytes * percent / 100;
        }

        progress = static_cast<double>(written) / static_cast<double>(totalBytes);
    }
    else if (m_totalItems > 0 && index > 0)
    {
        const double partOfCurrent = (percent > 0) ? (percent / 100.0) : 0.0;
        progress = (static_cast<double>(index - 1) + partOfCurrent) /
                   static_cast<double>(m_totalItems);
    }

    if (progress < 0.0)
    {
        progress = 0.0;
    }
    else if (progress > 1.0)
    {
        progress = 1.0;
    }

    int position = static_cast<int>(progress * 1000.0 + 0.5);

    if (position < 0)
    {
        position = 0;
    }
    else if (position > 1000)
    {
        position = 1000;
    }

    // ------------------------------------------------------------------
    // 进度条：直接重画那一小块，不再 invalidate
    //
    // 为什么不 InvalidateRect：状态栏一收到失效，会先把这一格"擦"回底色，
    // 然后我们才把条贴上去 —— 擦和贴之间有一下空档，看着就是闪（线密的地方
    // 对比最强，所以那里最明显）。改成直接 BitBlt 覆盖旧内容，就没有擦那一步。
    //
    // 兜底：万一状态栏被别的原因重绘（缩放窗口、被遮挡后又露出来），
    // 子类化的 WM_PAINT 里还会照旧补画一次，条不会消失。
    // ------------------------------------------------------------------
    m_position = position;
    Paint();

    // 任务栏按钮上的进度跟状态栏同步（0..1000）
    if (m_taskbar != nullptr && m_ownerWindow != nullptr)
    {
        m_taskbar->SetProgressValue(m_ownerWindow, static_cast<ULONGLONG>(position), 1000ULL);
    }

    // ------------------------------------------------------------------
    // 第 0 格：左边文字"正在粘贴：第 3/22 项"
    // ------------------------------------------------------------------
    std::wstring text = L"正在粘贴";

    if (m_totalItems > 0 && index > 0)
    {
        text += L"：第 " + std::to_wstring(index) + L"/" + std::to_wstring(m_totalItems) + L" 项";
    }

    if (text == m_progressText)
    {
        return;
    }

    m_progressText = text;
    ::SendMessageW(m_statusBar, SB_SETTEXTW, 0,
                   reinterpret_cast<LPARAM>(m_progressText.c_str()));
}

// ============================================================================
// 状态栏子类化
// ============================================================================

LRESULT CALLBACK PasteProgressBar::StatusBarSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
                                                         LPARAM lParam, UINT_PTR idSubclass,
                                                         DWORD_PTR refData)
{
    // 先让系统照常处理（画背景、写左右两格文字）
    const LRESULT result = ::DefSubclassProc(hwnd, msg, wParam, lParam);

    PasteProgressBar* self = reinterpret_cast<PasteProgressBar*>(refData);

    if (msg == WM_PAINT && self != nullptr)
    {
        self->Paint();
    }
    else if (msg == WM_NCDESTROY)
    {
        // 窗口要销毁了：把自己的子类化摘掉（标准收尾，否则会留悬挂的引用）
        ::RemoveWindowSubclass(hwnd, &PasteProgressBar::StatusBarSubclassProc, idSubclass);
    }

    return result;
}
