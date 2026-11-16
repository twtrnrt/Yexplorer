// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/BatchRenameDialog.h
//
// 「批量重命名」窗口（选中多个文件/文件夹后右键进来）。
//
// 【界面结构】
//   上半部分：预览列表（原名 → 新名 → 状态），规则改一下列表立刻重算；
//   中间参数区，三行：
//       行1  前缀 / 后缀 / 扩展名（不修改·修改 + 值）/ 大小写
//       行2  查找（右边一个「普通 / 正则」按钮，点开弹菜单选查找方式）/ 替换为 /
//            ☐含扩展名 / ☐区分大小写
//       行3  ☐加序号 + 起始 / 步长 / 位数 / 位置 / 分隔符
//   底部一条：左边排序 + ☐倒序，中间「共 N 项 · 会改动 M 项」，右边 取消 / 清空 / 保存 / 应用。
//
// 【风格】
//   和「设置」「打开方式」两个窗口一套：白底无边框客户区，只有系统控件
//   （编辑框、复选框、下拉框、列表）用原生的，按钮自己画。
//
// 【本阶段做到哪】
//   先把界面和"能看见效果"的预览做出来：
//     · 规则全部就位，任何改动都会即时算出新名字并刷到右边；
//     · 状态列会标出"未改动 / 已修改 / 冲突（重名、非法字符、空名）"；
//     · 点「应用」暂时只把结果回传（真正改名、撤销、进度条等下一步接）。
// ============================================================================

#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

// ----------------------------------------------------------------------------
// 一条待改名的项
// ----------------------------------------------------------------------------
struct BatchRenameItem
{
    std::wstring path;        // 完整路径（改名前）
    std::wstring name;        // 原文件名（含扩展名）
    bool         isFolder = false;
    uint64_t     size     = 0;   // 文件大小（排序用）
    FILETIME     modified = {};  // 修改时间（排序用）
};

// ----------------------------------------------------------------------------
// 应用后回传的一条结果
// ----------------------------------------------------------------------------
struct BatchRenameResult
{
    std::wstring oldPath;
    std::wstring newName;   // 只含文件名，不含目录
};

// ----------------------------------------------------------------------------
// 一套可保存的规则（"模板"）
//
// 把窗口里所有规则控件的样子原样记下来，存成一份；点「载入」时按字回填，
// 用户就不用每次都重新摆一遍。字段和界面上的控件一一对应。
// ----------------------------------------------------------------------------
struct RenameTemplate
{
    std::wstring name;

    std::wstring prefix;        // 前缀
    std::wstring suffix;        // 后缀
    std::wstring find;          // 查找
    std::wstring replace;       // 替换为
    bool         caseSensitive = false;   // 区分大小写
    bool         includeExt    = false;   // 查找替换是否连扩展名一起处理

    // 查找方式：0 = 普通查找；1~9 = 内置正则预设；10 = 自定义正则
    // （除了普通查找以外全都按正则解释）
    int          regexPreset   = 0;

    bool         number        = false;   // 加序号
    int          numberStart   = 1;       // 起始
    int          numberStep    = 1;       // 步长
    int          numberDigits  = 2;       // 位数
    int          numberPos     = 0;       // 0 = 放后面，1 = 放前面
    std::wstring numberSep     = L"-";    // 分隔符

    // 扩展名：0 不修改 / 1 修改（右边那个框填什么就用什么，留空等于去掉扩展名）
    // 2 是老模板里"去掉扩展名"那一项的旧值，界面上已经不提供了，读到就按"修改且清空"处理
    int          extMode       = 0;
    std::wstring extValue;                // 改成什么扩展名

    int          caseMode      = 0;       // 0 保持 / 1 大写 / 2 小写 / 3 首字母
    int          sortMode      = 0;       // 0 名称 / 1 时间 / 2 大小 / 3 自定义顺序
    bool         sortReverse   = false;   // 倒序
};

class BatchRenameDialog
{
public:
    BatchRenameDialog() = default;
    ~BatchRenameDialog();

    BatchRenameDialog(const BatchRenameDialog&) = delete;
    BatchRenameDialog& operator=(const BatchRenameDialog&) = delete;

    // 以模态方式打开。
    // paths：要改名的项的完整路径（调用方保证至少 2 项、且都是文件系统路径）
    // 返回 true 表示用户点了「应用」，results 里是每一项的目标文件名；
    // 返回 false 表示取消。
    bool ShowModal(HWND owner, const std::vector<std::wstring>& paths,
                   std::vector<BatchRenameResult>& results);

private:
    static LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    static bool EnsureClassRegistered();

    bool CreateDialogWindow();
    void Layout();
    void CreateControls();

    // 整窗背景 + 标题 + 分隔线 + 底部按钮
    void Draw(HDC dc);
    void DrawButton(HDC dc, const RECT& rect, const wchar_t* text, bool hot, bool pressed,
                    bool primary, bool enabled = true);
    void DrawTemplateSlot(HDC dc, const RECT& rect, int index);

    int Scale(int value) const;

    // 按当前规则算一遍新名字，并刷新右侧列表与底部统计
    void Recalculate();

    // 计算一项的新文件名（不含目录）
    std::wstring ComputeNewName(size_t index, const std::wstring& originalName) const;

    // 预览列表：填充所有行
    void FillPreviewList();

    // 状态文字与颜色（0 正常、1 已修改、2 冲突）
    struct PreviewRow
    {
        size_t       itemIndex = 0;   // 对应 m_items 里的下标（列表按排序显示，和这个不是一回事）
        std::wstring oldName;
        std::wstring newName;
        int          state = 0;       // 0 未改动 / 1 已修改 / 2 冲突
        std::wstring stateText;
    };

    // 读所有控件当前值，重算 rows
    void BuildPreviewRows();

    // 控件值读取小工具
    std::wstring GetTextOf(HWND control) const;
    int          ComboSelection(HWND control) const;

    void EnableRuleControls();

    // ---- 模板（固定 10 个槽，点一下载入，右键就地改名，改过不许直接应用）----
    static const size_t kTemplateSlotCount = 10;   // 模板槽固定 10 个，一行排满，不增不减

    void LoadTemplatesFromDisk();
    void SaveTemplatesToDisk() const;
    void EnsureTemplateSlots();                            // 保证 m_templates 正好 10 条
    void CaptureCurrentRules(RenameTemplate& out) const;   // 把界面上的规则收成一份模板
    void ApplyTemplateToControls(const RenameTemplate& tpl); // 把模板回填到控件
    void ClearAllRules();                                  // 「清空」：规则区全部回默认值

    // 选中的槽改过没有（改过的话「应用」变灰、必须先把「保存」点亮）
    bool HasSlotChanges() const;
    bool ApplyButtonEnabled() const;
    bool SaveCurrentSlot();            // 把规则区当前参数写进高亮的槽，成功返回 true
    bool ConfirmLeaveSlot();           // 切槽/关窗前问一句：true = 可以继续，false = 留在原处

    void OnTemplateSlotClick(int index);    // 左键点槽：高亮 + 载入
    void BeginTemplateRename(int index);    // 右键点槽：在按钮上原地改名
    void CommitTemplateRename();
    void CancelTemplateRename();

    // ---- 拖动排序（在预览列表里手动排）----
    void BeginListDrag(int itemIndex);
    void UpdateListDrag(int clientY);
    void FinishListDrag();

    // 正则下拉里的内置常用写法（下标 0 表示"普通查找"，1~9 是预设）
    static size_t RegexPresetCount();
    static const wchar_t* RegexPresetName(size_t index);
    static const wchar_t* RegexPresetPattern(size_t index);

    // 查找方式下拉里"自定义正则"那一项的编号（选它 = 用户在查找框里自己写正则）
    static const int kRegexCustomItem = 10;

    // 查找方式（点「普通 / 正则」按钮弹出菜单，选一条就生效）
    void OnRegexButtonClick();
    void SetRegexPreset(int preset);   // 改查找方式（选了内置正则会顺手把写法填进「查找」）

    // 当前是不是按正则查找（除了「普通查找」以外都算）
    bool RegexOn() const;

private:
    HWND m_hWnd   = nullptr;
    HWND m_hOwner = nullptr;

    std::vector<BatchRenameItem> m_items;
    std::vector<PreviewRow>      m_rows;
    std::vector<size_t>          m_order;      // 当前显示顺序（元素是 m_items 的下标）

    // ---- 正则 ----
    bool m_regexValid = true;   // 当前正则能不能编译通过

    // 一次性回填很多控件时（载入模板）先把这个置 true，
    // 免得每设一个框都触发一遍重算
    bool m_suppressRecalculate = false;

    // ---- 控件句柄 ----
    HWND m_hList = nullptr;         // 预览列表（report 模式的 ListView）

    HWND m_hPrefix = nullptr;       // 前缀
    HWND m_hSuffix = nullptr;       // 后缀
    HWND m_hFind = nullptr;         // 查找
    HWND m_hReplace = nullptr;      // 替换为
    HWND m_hCaseSensitive = nullptr; // 区分大小写（复选框）
    HWND m_hIncludeExt = nullptr;   // 含扩展名（复选框：查找替换是否连扩展名一起处理）

    // 查找方式（不再用下拉框，改成「普通 / 正则」按钮 + 弹出菜单）：
    // 0 = 普通查找；1~9 = 内置正则；10 = 自定义正则
    int  m_regexPreset       = 0;
    RECT m_regexButtonRect   = {};
    bool m_hotRegexButton    = false;
    bool m_pressedRegexButton = false;

    // ---- 模板栏：固定 10 个自绘按钮，一行排满 ----
    std::vector<RenameTemplate> m_templates;        // 恒为 kTemplateSlotCount 条
    std::vector<RECT>           m_templateRects;    // 10 个槽的矩形
    int  m_hotTemplate     = -1;   // 鼠标悬停的槽（-1 = 没有）
    int  m_pressedTemplate = -1;   // 正按下的槽
    int  m_selectedTemplate = -1;  // 当前高亮（载入）的槽，-1 = 没选中任何模板

    // 选中槽"上次保存/载入时的样子"，用来判断有没有改动（名字不参与比较）
    RenameTemplate m_baseline;
    bool           m_hasBaseline = false;

    // 右键就地改名用的输入框（叠在槽按钮上，回车/失焦提交、Esc 放弃）
    HWND m_hRenameEdit = nullptr;
    int  m_renameIndex = -1;

    HWND m_hNumberEnable = nullptr; // 加序号（复选框）
    HWND m_hNumberStart = nullptr;  // 起始
    HWND m_hNumberStep = nullptr;   // 步长
    HWND m_hNumberDigits = nullptr; // 位数
    HWND m_hNumberPos = nullptr;    // 位置（前缀 / 后缀）
    HWND m_hNumberSep = nullptr;    // 分隔符

    HWND m_hExtMode = nullptr;      // 扩展名：保留 / 改为 / 删除
    HWND m_hExtValue = nullptr;     // 扩展名改成什么

    HWND m_hCaseMode = nullptr;     // 大小写：保持 / 大写 / 小写 / 首字母大写

    HWND m_hSortMode = nullptr;     // 排序：名称 / 修改时间 / 大小 / 自定义顺序（在底部一行）
    HWND m_hSortReverse = nullptr;  // 反向（复选框，在底部一行）

    // ---- 预览列表的拖动排序状态 ----
    bool                m_dragging      = false;  // 正拖着
    std::vector<size_t> m_dragRows;               // 被拖的行（m_rows 的下标）
    int                 m_dropRow       = -1;     // 要插到第几行前面（-1 = 没算出位置）
    bool                m_dropAfter     = false;  // 插在该行后面

    // ---- 自绘按钮（底部一排：取消 / 清空 / 保存 / 应用）----
    RECT m_cancelRect = {};
    RECT m_clearRect  = {};
    RECT m_saveRect   = {};
    RECT m_applyRect  = {};
    bool m_hotCancel  = false;
    bool m_hotClear   = false;
    bool m_hotSave    = false;
    bool m_hotApply   = false;
    bool m_pressedCancel = false;
    bool m_pressedClear  = false;
    bool m_pressedSave   = false;
    bool m_pressedApply  = false;
    bool m_trackingLeave = false;

    // 底部统计文字的矩形（每次只重画这一块，避免整窗闪）
    RECT m_statsRect = {};

    // ---- 字体 ----
    HFONT m_font       = nullptr;
    HFONT m_fontTitle  = nullptr;
    HFONT m_fontSmall  = nullptr;   // 底部提示用
    HBRUSH m_whiteBrush = nullptr;
    HBRUSH m_disabledBrush = nullptr;   // 被禁用的输入框用这个灰底

    // ---- 结果 ----
    bool m_applied = false;    // 用户点了「应用」
};
