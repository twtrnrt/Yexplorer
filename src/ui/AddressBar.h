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

    // 整条显示 label，不按反斜杠切段。
    // 两种情况用得到：
    //   1) 虚拟文件夹（「此电脑」这种），路径是 ::{GUID}，没法切段；
    //   2) 从「主文件夹」双击进去的系统目录（文档、图片、下载…），
    //      用户希望地址栏就显示「文档」，而不是 C:\Users\xxx\Documents。
    // 点击整段仍然回到 path 本身。
    void SetPathWithLabel(const std::wstring& path,
                          const std::wstring& label,
                          bool              wholeLabel = false);

    // 带"折叠前缀"的面包屑：path 位于 prefixPath 之内时，
    // 第一段显示 prefixLabel（点它回 prefixPath），后面再把 prefixPath
    // 之后的部分一段段切开。
    //   例：path = C:\Users\user\Downloads\2025
    //       prefixPath = C:\Users\user\Downloads，prefixLabel = 下载
    //       → 「下载 > 2025」，点「下载」回下载目录
    // 这种写法只用在系统已知文件夹（下载/图片/文档…）的子目录上；
    // 普通目录还是走 SetPath，显示完整路径。
    void SetPathWithPrefix(const std::wstring& path,
                           const std::wstring& prefixPath,
                           const std::wstring& prefixLabel);

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

    // 段之间的那个小箭头：一次性画成一张带 alpha 的位图（手工抗锯齿、上下对称），
    // 之后每次绘制只是贴图。DPI 缩放变了尺寸不对就重建。
    void EnsureSeparatorBitmap();

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
    std::wstring m_label;                         // 虚拟文件夹的显示名（如「此电脑」），普通路径为空
    bool         m_wholeLabel = false;            // 是否整条只显示 m_label（不切成一段段）
    std::wstring m_prefixPath;                    // 折叠前缀：那个系统文件夹的真实路径（如 ...\Downloads）
    std::wstring m_prefixLabel;                   // 折叠前缀显示的名字（如「下载」），空表示不折叠
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

    // 分隔符箭头用的位图（32 位带 alpha，见 EnsureSeparatorBitmap）
    HBITMAP m_separatorBitmap    = nullptr;
    int     m_separatorBmpWidth  = 0;
    int     m_separatorBmpHeight = 0;

    std::function<void(const std::wstring&)> m_navigateCallback;
};
