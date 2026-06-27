#pragma once

#include <Veng/Veng.h>

#include <deque>

namespace VengEditor
{
    class EditorCommand;
    struct PrefabEditContext;

    /// @brief Per-document undo/redo history of EditorCommands.
    ///
    /// One CommandStack lives on each AssetEditorPanel, so two open documents undo independently.
    /// It holds an undo deque and a redo deque over the document's PrefabEditContext: Push applies
    /// a command and clears redo; Undo reverts the top and moves it to redo; Redo re-applies and
    /// moves it back. The undo deque is capped at a fixed depth (the oldest entries drop). A dirty
    /// signal (set by any Push/Undo/Redo, cleared by MarkSaved) tracks unsaved changes for a save
    /// path; selection is editor state and never enters the stack.
    class CommandStack
    {
    public:
        /// @brief Maximum number of undo entries retained; older edits drop off the bottom.
        static constexpr Veng::usize MaxDepth = 128;

        /// @brief Constructs the stack over the document edit context it mutates.
        /// @param ctx  The document context (Scene + AssetManager) commands run against.
        explicit CommandStack(PrefabEditContext& ctx);
        ~CommandStack();

        CommandStack(const CommandStack&) = delete;
        CommandStack& operator=(const CommandStack&) = delete;

        /// @brief Applies @p command, pushes it onto the undo stack, and clears the redo stack.
        ///
        /// The command's Apply runs immediately. The redo stack is cleared because a fresh edit
        /// diverges from any previously undone branch. Marks the stack dirty.
        /// @param command  The command to apply and record; ownership is transferred.
        void Push(Veng::Unique<EditorCommand> command);

        /// @brief Reverts the most recent command and moves it to the redo stack. No-op when empty.
        void Undo();

        /// @brief Re-applies the most recently undone command and moves it back. No-op when empty.
        void Redo();

        /// @brief Returns true when there is a command to undo.
        [[nodiscard]] bool CanUndo() const { return !m_Undo.empty(); }

        /// @brief Returns true when there is a command to redo.
        [[nodiscard]] bool CanRedo() const { return !m_Redo.empty(); }

        /// @brief Returns the title of the next command Undo would revert, or empty when none.
        [[nodiscard]] Veng::string_view UndoTitle() const;

        /// @brief Returns the title of the next command Redo would re-apply, or empty when none.
        [[nodiscard]] Veng::string_view RedoTitle() const;

        /// @brief Returns true when the document has unsaved edits since the last MarkSaved.
        [[nodiscard]] bool IsDirty() const { return m_Dirty; }

        /// @brief Clears the dirty flag — the document's current state is the saved state.
        void MarkSaved() { m_Dirty = false; }

    private:
        PrefabEditContext& m_Ctx;
        std::deque<Veng::Unique<EditorCommand>> m_Undo;
        std::deque<Veng::Unique<EditorCommand>> m_Redo;
        bool m_Dirty = false;
    };
}
