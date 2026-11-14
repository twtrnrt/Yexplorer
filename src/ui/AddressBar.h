// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/AddressBar.h
//
// 地址栏（面包屑式）。
//
// 【为什么自己做】
//   系统原生资源管理器的地址栏不是普通输入框，而是把路径切成一段一段
//   （比如  C: > Windows > System32 ），鼠标点哪一段就直接跳过去，
//   点最右边的空白处才变成可输入的文本框。
//   普通 EDIT 控件做不到这一点，所以这里自绘一个。
//
// 【交互】
//   · 平时：显示分段路径，每段可单独点击跳转，鼠标移上去有浅灰圆角背景
//   · 点分段之间的空白：切换成输入框，显示完整路径，可以直接打字
//   · 输入框里回车：跳转；按 Esc：放弃输入，退回分段显示
// ============================================================================

#pragma once

#include <windows.h>
#include <functional>
#include <string>
#include <vector>

class AddressBar
{
public:
    AddressBar() = default;
    ~AddressBar();

    AddressBar(const AddressBar&) = delete;
    AddressBar& operator=(const AddressBar&) = delete;

    bool Create(HWND parent, UINT id, HINSTANCE hInstance);

    void Resize(int x, int y, int width, int height);

    // 外部导航到新目录后调用，重新生成分段
    void SetPath(const std::wstring& path);

    // 点了某一段时回调，参数是该段对应的完整路径
    void SetNavigateCallback(std::function<void(const std::wstring&)> callback);

    // 使用主窗口的统一字体
    void SetFont(HFONT font);

    HWND Handle() const { return m_hWnd; }

    // 当前显示/正在编辑的路径（给键盘快捷键用，比如 Alt+D 后取内容）
    std::wstring CurrentPath() const { return m_path; }

    // 进入手动输入模式（供快捷键调用）
    void BeginEditing();

private:
    // 一段路径：显示文字 + 点击后跳转的完整路径
    struct Segment
    {
        std::wstring text;
        std::wstring fullPath;
    };

    static LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                             UINT_PTR idSubclass, DWORD_PTR refData);

    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void RebuildSegments();
    void LayoutSegments(HDC hdc);

    void Draw(HDC hdc);

    void BeginEditingImpl();
    void EndEditing();
    void CommitEditing();

    int  Scale(int value) const;

    static bool EnsureClassRegistered(HINSTANCE hInstance);

private:
    HWND m_hWnd  = nullptr;
    UINT m_controlId = 0;

    std::wstring m_path;                          // 当前路径
    std::vector<Segment> m_segments;              // 切好的分段
    std::vector<RECT>    m_segmentRects;          // 每段在控件内的矩形（用于命中判断）
    std::vector<bool>    m_segmentVisible;        // 该段是否被省略掉
    RECT                 m_ellipsisRect = { 0, 0, 0, 0 };   // 中间省略号"…"的位置

    int  m_hotIndex = -1;                         // 鼠标悬停在第几段
    bool m_trackingLeave = false;                 // 是否已注册"鼠标离开"通知

    bool m_editing  = false;                      // 是否处于输入模式
    HWND m_hEdit    = nullptr;                    // 输入框（按需创建，平时隐藏）

    HFONT m_hFont   = nullptr;                    // 路径文字字体
    HFONT m_hOwnFont = nullptr;                   // 如果外部没给字体，自己建一个

    std::function<void(const std::wstring&)> m_navigateCallback;
};
