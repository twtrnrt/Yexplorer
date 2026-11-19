// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/core/UndoStack.cpp
//
// 撤销 / 重做栈的实现。语义见 UndoStack.h。
// ============================================================================

#include "core/UndoStack.h"

namespace core
{

void UndoStack::PushCapped(std::vector<UndoRecord>& stack, const UndoRecord& record)
{
    stack.push_back(record);

    // 超出上限就把最老的那一步挤掉
    if (stack.size() > kMaxSteps)
    {
        stack.erase(stack.begin());
    }
}

void UndoStack::Push(const UndoRecord& record)
{
    if (record.kind == UndoKind::None)
    {
        return;
    }

    PushCapped(m_undo, record);

    // 出现新操作后，原来"恢复"过的那些步骤就作废了（和所有编辑器一样）
    m_redo.clear();
}

void UndoStack::PushRedo(const UndoRecord& record)
{
    PushCapped(m_redo, record);
}

void UndoStack::PushUndoOnly(const UndoRecord& record)
{
    PushCapped(m_undo, record);
}

UndoRecord UndoStack::TakeUndo()
{
    UndoRecord record;

    if (!m_undo.empty())
    {
        record = std::move(m_undo.back());
        m_undo.pop_back();
    }

    return record;
}

UndoRecord UndoStack::TakeRedo()
{
    UndoRecord record;

    if (!m_redo.empty())
    {
        record = std::move(m_redo.back());
        m_redo.pop_back();
    }

    return record;
}

void UndoStack::Clear()
{
    m_undo.clear();
    m_redo.clear();
}

const wchar_t* UndoStack::VerbText(UndoKind kind)
{
    switch (kind)
    {
    case UndoKind::Create: return L"新建";
    case UndoKind::Rename: return L"重命名";
    case UndoKind::Move:   return L"移动";
    case UndoKind::Copy:   return L"复制";
    case UndoKind::Delete: return L"删除";
    default:               return nullptr;
    }
}

} // namespace core
