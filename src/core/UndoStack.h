// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/core/UndoStack.h
//
// 撤销 / 恢复的"栈"本身。
//
// 【为什么单独一个文件】
//   两个栈、50 级上限、"新操作清空重做栈"这几条规则原来散在 MainWnd 里，
//   和"撤销要执行什么文件操作"混在一起。把栈抽出来以后：
//     · 规则只有一处，改上限/语义不用翻 MainWnd；
//     · 执行侧（PerformUndo / PerformRedo）仍留在 MainWnd，因为它要调 Shell 操作。
//
// 【语义】（和资源管理器 / 常见编辑器一致）
//   新操作 Push：进撤销栈，同时清空重做栈；
//   撤销 TakeUndo：从撤销栈拿走一步，执行完由调用方 PushRedo；
//   重做 TakeRedo：从重做栈拿走一步，执行完由调用方 PushUndoOnly（不动重做栈）。
// ============================================================================

#pragma once

#include <string>
#include <vector>

namespace core
{

// 一步操作的种类（决定撤销/重做时该调哪套文件操作）
enum class UndoKind
{
    None,
    Create,     // 新建（文件夹 / 新建菜单里建出来的文件）
    Rename,     // 重命名
    Move,       // 剪切粘贴（移动）
    Copy,       // 复制粘贴
    Delete,     // 删除（进回收站，撤销 = 从回收站还原）
};

// 一步操作的记录：操作前在哪、操作后在哪
struct UndoRecord
{
    UndoKind                  kind = UndoKind::None;
    std::vector<std::wstring> afterPaths;    // 操作之后这些项在哪（撤销要动它们）
    std::vector<std::wstring> beforePaths;   // 操作之前在哪（重命名前的全路径 / 移动和删除的源路径）

    // 批量重命名专用：这条照样在撤销栈里、右键「撤销」能撤，
    // 但 Ctrl+Z / Ctrl+Y 会被拦掉（用户拍板：批量改名不能走快捷键）。
    bool batchRename = false;
};

class UndoStack
{
public:
    // 最多记多少级（再多也没人真的会连撤 50 步，省点内存）
    static const size_t kMaxSteps = 50;

    // 新操作：进撤销栈、清空重做栈、超上限挤掉最老的
    void Push(const UndoRecord& record);

    // 撤销之后：把刚撤掉的那一步压进重做栈
    void PushRedo(const UndoRecord& record);

    // 重做之后：把刚重做的那一步压回撤销栈（不动重做栈）
    void PushUndoOnly(const UndoRecord& record);

    bool CanUndo() const { return !m_undo.empty(); }
    bool CanRedo() const { return !m_redo.empty(); }

    // 栈顶那一步（空栈返回 nullptr；只读，不拿走）
    const UndoRecord* TopUndo() const { return m_undo.empty() ? nullptr : &m_undo.back(); }
    const UndoRecord* TopRedo() const { return m_redo.empty() ? nullptr : &m_redo.back(); }

    // 拿走栈顶（调用方执行完再 PushRedo / PushUndoOnly）
    UndoRecord TakeUndo();
    UndoRecord TakeRedo();

    void ClearRedo() { m_redo.clear(); }
    void Clear();

    // 把一步操作的动词转成菜单文案里的字（新建 / 重命名 / 移动 / 复制 / 删除）
    static const wchar_t* VerbText(UndoKind kind);

private:
    static void PushCapped(std::vector<UndoRecord>& stack, const UndoRecord& record);

    std::vector<UndoRecord> m_undo;   // 撤销栈（末尾是最新的一步）
    std::vector<UndoRecord> m_redo;   // 重做栈（撤销过的步骤按顺序排在这里）
};

} // namespace core
