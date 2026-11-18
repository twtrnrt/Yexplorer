// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ShellClipboard.h
//
// 系统剪贴板：文件和资源管理器互通。
//
// 【为什么不能只记在程序内存里】
//   只记在自己内存里的话，从资源管理器复制的东西粘不进来，
//   在本程序复制的东西也粘不出去。用户会立刻觉得"这软件不全"。
//
// 【用的什么格式】
//   CF_HDROP：Windows 放"一堆文件路径"的标准格式，资源管理器、
//             桌面、各种程序都认它，就这一个格式就能打通绝大部分场景。
//   "Preferred DropEffect"：一个 DWORD，告诉接收方这次是"复制"还是"剪切"。
//             没有它，接收方默认按复制处理，剪切就失效了。
//             （资源管理器按 Ctrl+X 时放的是 DROPEFFECT_MOVE）
// ============================================================================

#pragma once

#include <windows.h>
#include <functional>
#include <string>
#include <vector>

// IDataObject 是 COM 接口，真正定义在 objidl.h（实现文件里会 include ole2.h）。
// 这里只需要指针，前置声明就够，免得头文件把整个 OLE 拖进来。
struct IDataObject;

namespace shellclip
{

// 把一批路径放进系统剪贴板
// isCut：true = 剪切（接收方粘贴完会删源），false = 复制
bool WriteFiles(const std::vector<std::wstring>& paths, bool isCut, HWND owner);

// 把一段纯文字放进系统剪贴板（CF_UNICODETEXT）。
// 现在给右键「复制文件路径」用：多选时几条路径按行拼成一段文字。
bool WriteText(const std::wstring& text, HWND owner);

// 从系统剪贴板读出一批文件路径
// 剪贴板里不是文件（比如是一段文字）就返回 false
// isCut：读到的"首选效果"是不是移动
bool ReadFiles(std::vector<std::wstring>& paths, bool& isCut, HWND owner);

// 剪贴板里现在有没有文件（右键菜单判断"粘贴"要不要变灰用）
// 有真实路径（CF_HDROP）算有；只有"虚拟文件"也算有
bool HasFiles();

// 清空剪贴板（剪切粘贴成功之后用，和资源管理器行为一致）
bool Clear(HWND owner);

// ============================================================================
// 「虚拟文件」：剪贴板里没有真实路径，只给"名字 + 内容流"的那一类
//
// 从哪来：
//   远程桌面（RDP / 3389）里复制文件 —— 文件还在远程电脑的硬盘上，
//     本机没有它们的路径，rdpclip.exe 往剪贴板里放的是
//     FileGroupDescriptorW（一张清单：名字/大小/属性/时间）
//     加 FileContents（内容流：要第几个就现传第几个）。
//   Outlook 里复制邮件附件、部分压缩软件的"虚拟复制"也是同一套格式。
//
// 资源管理器认这套格式，所以能粘；我们原来只读 CF_HDROP（真实路径），
// 读不到就当成"剪贴板里没文件"，所以粘不了。
// ============================================================================

// 一个虚拟文件的描述（只有元信息，内容在需要时才去取）
struct VirtualFileInfo
{
    // 相对路径。RDP 复制整个文件夹时会带 "子目录\文件" 这种形式，
    // 分隔符统一成反斜杠，非法的字符已经清理掉（见实现里的清理函数）
    std::wstring name;

    DWORD    attributes = 0;      // 文件属性（只读/隐藏/是不是目录）
    LONGLONG size       = -1;     // 字节数；-1 = 来源没告诉
    FILETIME lastWriteTime = {};  // 修改时间

    // 清单里的对应项"有没有说"这三个信息（没说就不要往目标文件上套）
    bool hasAttributes    = false;
    bool hasSize          = false;
    bool hasLastWriteTime = false;
};

// 剪贴板里现在是不是这种"虚拟文件"（没有真实路径）
// 只查剪贴板格式，不取数据、不起 COM，右键菜单里调用代价很小
bool HasVirtualFiles();

// 一次粘贴的读取会话：
//   Open   取一次 IDataObject 并把清单解析出来（之后不再反复问剪贴板）
//   Extract 按索引把第几个文件的内容写到指定路径
//
// 必须在初始化过 COM 的线程里用（主窗口的后台文件操作线程是 STA，正合适）。
class VirtualFileSession
{
public:
    VirtualFileSession();
    ~VirtualFileSession();

    VirtualFileSession(const VirtualFileSession&) = delete;
    VirtualFileSession& operator=(const VirtualFileSession&) = delete;

    // 读剪贴板拿数据对象 + 解析清单。失败时 LastError() 里是中文原因。
    bool   Open();
    void   Close();
    bool   IsOpen() const;

    size_t Count() const;
    const  VirtualFileInfo& Item(size_t index) const;

    const std::wstring& LastError() const;

    // 进度回调：percent 是"这一个文件"的进度（0..100；来源没告诉大小时为 -1），
    // 返回 false 表示用户取消，Extract 会把写了一半的文件删掉再返回失败。
    using ProgressCallback = std::function<bool(int percent)>;

    // 把第 index 项写到 destinationPath（父目录必须已经存在）。
    // 清单里的"目录项"不取内容，直接建目录。
    // 成功后按清单补上属性和修改时间。
    bool Extract(size_t index, const std::wstring& destinationPath,
                 const ProgressCallback& onProgress, std::wstring& error);

private:
    IDataObject*                 m_dataObject = nullptr;   // 剪贴板数据对象（自己 AddRef 的）
    std::vector<VirtualFileInfo> m_files;                  // 解析出来的清单
    std::wstring                 m_lastError;

    bool ReadDescriptor(bool wideVersion);   // 读清单，wideVersion 决定用 W 还是 A 格式
};

} // namespace shellclip
