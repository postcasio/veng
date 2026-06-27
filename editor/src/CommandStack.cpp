#include "CommandStack.h"

#include "EditorCommand.h"

namespace VengEditor
{
    using namespace Veng;

    CommandStack::CommandStack(PrefabEditContext& ctx) : m_Ctx(ctx) {}

    CommandStack::~CommandStack() = default;

    void CommandStack::Push(Unique<EditorCommand> command)
    {
        command->Apply(m_Ctx);
        m_Undo.push_back(std::move(command));

        // A fresh edit diverges from any undone branch, so the redo history is no longer reachable.
        m_Redo.clear();

        // Bound the history; the oldest edit drops off the bottom past the cap.
        if (m_Undo.size() > MaxDepth)
        {
            m_Undo.pop_front();
        }

        m_Dirty = true;
    }

    void CommandStack::Undo()
    {
        if (m_Undo.empty())
        {
            return;
        }
        Unique<EditorCommand> command = std::move(m_Undo.back());
        m_Undo.pop_back();
        command->Revert(m_Ctx);
        m_Redo.push_back(std::move(command));
        m_Dirty = true;
    }

    void CommandStack::Redo()
    {
        if (m_Redo.empty())
        {
            return;
        }
        Unique<EditorCommand> command = std::move(m_Redo.back());
        m_Redo.pop_back();
        command->Apply(m_Ctx);
        m_Undo.push_back(std::move(command));
        m_Dirty = true;
    }

    string_view CommandStack::UndoTitle() const
    {
        return m_Undo.empty() ? string_view{} : m_Undo.back()->GetTitle();
    }

    string_view CommandStack::RedoTitle() const
    {
        return m_Redo.empty() ? string_view{} : m_Redo.back()->GetTitle();
    }
}
