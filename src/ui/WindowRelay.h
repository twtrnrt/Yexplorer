#pragma once

#include <string>
#include <windows.h>

// ============================================================================
// 窗口之间"这个目录我已经开着了"的接力（跨进程）
//
// 背景：本程序的「在新窗口中打开」是再启动一个自己（新进程），所以桌面可能同时
// 有好几个窗口。用户要求：打开某个文件夹时，如果已经有一个窗口停在这个目录，
// 就把那个窗口拉到前台，不要再开一个。
//
// 做法：新进程（或当前窗口）先枚举同类名的顶层窗口，逐个发一条 WM_COPYDATA
// 问一句"你是不是正停在这个目录？"，对方比对当前目录，命中就还原 + 置前 + 回 1，
// 发送方收到 1 就放弃开新窗口。
//
// 成本：只在"要开一个新窗口"的那一下做一次枚举加一条同步消息（2 秒超时保护），
// 日常导航、右键菜单完全不走这条路，不影响"右键零成本"那条规矩。
// ============================================================================

namespace ui
{

// 主窗口类名。放在这里是因为跨进程找窗口要用它，必须和 MainWnd.cpp 注册的一致。
extern const wchar_t kMainWindowClassName[];

// WM_COPYDATA 的 dwData 标记，用来认出"这是本程序的接力请求"
constexpr DWORD kRelayRequestMagic = 0x59457831;   // 'YEx1'

// 跨进程传递的请求体。字段用定长数组而不是指针：WM_COPYDATA 会把这段内存
// 拷到对方进程，指针过去就失效了。
struct RelayRequest
{
    wchar_t folder[520];      // 目标目录（已规范化，见 NormalizeFolderForCompare）
    wchar_t selectFile[520];  // 要在该目录里选中的文件（空 = 只拉窗口，不选中）
};

// 规范化目录路径，用于"是不是同一个目录"的比较：
//   普通磁盘路径先去尾斜杠（但 "C:\" 这种根保留），虚拟位置（:: 开头、shell: 开头）原样；
// 不调 COM、不碰文件系统，纯字符串处理。
std::wstring NormalizeFolderForCompare(const std::wstring& path);

// 两个目录是不是同一处（忽略大小写和尾斜杠）
bool IsSameFolder(const std::wstring& left, const std::wstring& right);

// 把目录（可选带要选中的文件）交给已经打开它的窗口。
// 返回 true = 已经有窗口接下了（调用方不要再开新窗口）。
// selfWindow 是调用方自己的窗口，一并参与匹配：同目录就在自己这里时，直接把自己置前。
bool RelayOpenFolderToExistingWindow(const std::wstring& folder,
                                     const std::wstring& selectFile,
                                     HWND selfWindow);

}   // namespace ui
