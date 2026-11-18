// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ShellDragDrop.h
//
// 拖放（拖出 + 拖入）的公共封装。
//
// 拖放这事儿必须走 OLE：Windows 上「把文件从一个窗口拖到另一个窗口」是标准机制，
// 参与的双方一边是「拖动源」（提供数据 + 决定流程），一边是「放置目标」（接收数据）。
//
//   拖出：构造一个装着文件列表的 IDataObject，再配一个 IDropSource，
//         调 DoDragDrop 进入系统拖动循环（这时是阻塞的，拖完才返回）。
//         这样拖到资源管理器、桌面、微信、浏览器……都能正常落地。
//
//   拖入：把自己的窗口注册成 IDropTarget。别人的资源管理器往我们的列表里拖文件时，
//         系统会把数据交给我们的对象处理。
// ============================================================================
#pragma once

#include <windows.h>
#include <ole2.h>        // DROPEFFECT / IDropSource / IDropTarget（WIN32_LEAN_AND_MEAN 下不会自动带）
#include <shlobj.h>
#include <shobjidl.h>

#include <functional>
#include <string>
#include <vector>

namespace shell
{

// ----------------------------------------------------------------------------
// 拖出：把一组文件/文件夹拖到别处
//
//   paths          ：要拖走的东西（绝对路径）
//   sourceWindow   ：发起拖动的窗口（用来定位拖动时跟着鼠标的那个图像）
//   dragPointClient：鼠标按下时的客户区坐标（同上，用于取拖动图像）
//   allowedEffects ：允许的效果位（复制/移动）
//   preferredEffect：首选效果（0 表示不声明）。
//                    从压缩包里往外拖时传 DROPEFFECT_COPY —— 源是解出来的临时副本，
//                    不该被"移动"掉；声明之后接收方（含我们自己的窗口）会按复制处理。
//
// 返回最终效果（DROPEFFECT_MOVE / COPY / NONE 这些宏值，类型是 DWORD）。
// 注意：这是阻塞调用，拖动过程中不要指望能执行别的代码。
// ----------------------------------------------------------------------------
DWORD DoDragFiles(const std::vector<std::wstring>& paths,
                  HWND                            sourceWindow,
                  POINT                           dragPointClient,
                  DWORD                           allowedEffects,
                  DWORD                           preferredEffect = 0);

// ----------------------------------------------------------------------------
// 拖入：接收别的程序拖过来的文件
//
// 一个窗口注册一次（列表控件、左栏树各注册自己的）。
// 窗口销毁前要 UnregisterWindowDropTarget，否则系统里还留着引用。
// ----------------------------------------------------------------------------
struct DropTargetCallbacks
{
    // 根据客户区坐标算出「要放到哪个目录里」
    //   列表控件 → 永远是当前目录
    //   左栏树   → 坐标落在哪个节点就用哪个节点的路径（落在文件夹节点上才有意义）
    // 返回空串表示这里不接受放置
    std::function<std::wstring(POINT ptClient)> ResolveTargetDirectory;

    // 真正执行放置
    //   sources         ：被拖来的文件完整路径
    //   targetDirectory ：落点目录
    //   effect          ：最终效果（DROPEFFECT_MOVE / COPY / LINK）
    //   createShortcut  ：是否按住 Alt（要在目标目录里建快捷方式）
    std::function<void(const std::vector<std::wstring>& sources,
                       const std::wstring&              targetDirectory,
                       DWORD                            effect,
                       bool                             createShortcut)> PerformDrop;

    // 拖动过程中的反馈（可选）
    //   ptClient   ：鼠标的客户区坐标
    //   acceptable ：这个位置能不能放
    //
    // 文件列表用它把"鼠标正压在哪个文件夹上"高亮出来（和资源管理器一样），
    // 鼠标离开列表时也会回调一次（acceptable = false），用来清掉高亮。
    std::function<void(POINT ptClient, bool acceptable)> DragOverFeedback;

    // 落点提示的自定义说法（可选）
    //   ptScreen   ：鼠标的屏幕坐标
    //   actionText ：例如「添加到」
    //   targetName ：例如「常用」
    //
    // 不是所有落点都等于"复制到某个目录"：比如拖进左栏空白处，语义是"添加一个常用项"。
    // 返回 true 时提示条就按这里给的说法显示，不再自己拼「复制到 xxx」。
    std::function<bool(POINT ptScreen, std::wstring& actionText, std::wstring& targetName)> ResolveDropHint;
};

// 注册成放置目标；成功返回 true
bool RegisterWindowDropTarget(HWND hwnd, const DropTargetCallbacks& callbacks);

// 注销放置目标
void UnregisterWindowDropTarget(HWND hwnd);

// ----------------------------------------------------------------------------
// 拖入效果计算（对外暴露，方便单元验证）：
//   keyState           ：拖动时的修饰键状态（MK_CONTROL / MK_SHIFT / MK_ALT）
//   sourcePreferredEffect ：拖动源声明的"首选效果"（Preferred DropEffect 格式）
//   sameDrive          ：源和目标是不是同一个盘
//   sourceIsDrive      ：拖动源是不是驱动器（盘符根，如 C:\、D:\）
//
// 与资源管理器一致：Ctrl→复制，Shift→移动，Alt→快捷方式，都不按则同盘移动、跨盘复制。
// 唯一的例外是源为驱动器：盘符搬不走、整盘复制也没意义，所以什么都不按也按
// 快捷方式处理（效果等同于按住 Alt 拖），和系统资源管理器拖盘符的表现一致。
// ----------------------------------------------------------------------------
DWORD ComputeDropEffect(DWORD keyState, DWORD sourcePreferredEffect, bool sameDrive,
                        bool sourceIsDrive);

// 判断两个路径是否在同一个盘（网络位置按"根路径"比较）
bool IsSameDrive(const std::wstring& pathA, const std::wstring& pathB);

// ----------------------------------------------------------------------------
// 判断 candidate 是不是 ancestor 本身、或者就在 ancestor 里面（子孙目录）
//
// 为什么需要它：把文件夹 A 放到 A 自己身上、或者放进 A 里的子文件夹，都是无效落点——
// 走到系统文件操作（IFileOperation）那一步会报「不能把文件夹移到自身/子文件夹」，
// 然后系统自己弹一个错误框。所以复制/移动要在拖动阶段就判死。
//
// 注意：建快捷方式（Alt 拖动）不受此限，在自己里面建一个指向自己的 .lnk 是合法的。
//
// 大小写不敏感；末尾反斜杠忽略（"C:\A" 与 "C:\A\" 视为同一个路径）。
// ----------------------------------------------------------------------------
bool IsSelfOrDescendantPath(const std::wstring& candidate, const std::wstring& ancestor);

} // namespace shell
