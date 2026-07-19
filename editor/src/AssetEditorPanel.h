#pragma once

#include <Veng/Result.h>
#include <Veng/Veng.h>

#include <VengEditor/EditorPanel.h>

namespace Veng
{
    class Scene;

    namespace Renderer
    {
        class Viewport;
    }
}

namespace VengEditor
{
    class CommandStack;

    /// @brief Base for an asset editor: the explicit-save contract, and a private dockspace.
    ///
    /// **An asset editor writes its source only from Save().** No auto-save and no timer: an edit
    /// accumulates in the panel's in-memory document and marks it dirty, and Save() performs the
    /// preserve-unknown-keys merge write, then the recook — in that order, so a cook that fails
    /// leaves the saved source on disk and reports in-panel rather than reverting the edit. Every
    /// asset editor derives from this base; that is what routes the File menu, Ctrl/Cmd+S, the
    /// Save action's enabled state, the title's unsaved marker, and the close prompt to it.
    ///
    /// An asset editor is a top-level panel in the host dockspace whose window hosts a
    /// per-instance ImGui dockspace. Its child panels are submitted as separate windows
    /// tagged with a per-instance window class, so only this editor's children dock into
    /// its dockspace and they cannot stray into the main host dockspace. The child set is
    /// fixed at construction; a subclass adds children with AddChild and arranges their
    /// initial split in BuildDefaultLayout.
    class AssetEditorPanel : public EditorPanel
    {
    public:
        /// @brief Submits the document window (with its dockspace and children) and the close prompt.
        ///
        /// An editor that added children hosts its private dockspace; a childless one is a plain
        /// single window. Either way, closing a document reporting HasUnsavedChanges() takes the
        /// close back and raises a modal Save / Discard / Cancel prompt rather than dropping the
        /// edits — the host destroys a panel whose open flag clears, so this is the only place to
        /// ask, and the prompt is modal so a stray click cannot leave the question unanswered.
        /// @param open  The host's visibility flag; cleared only once a close is confirmed.
        void Draw(bool* open) override;

        /// @brief The document body drawn inside the dockspace host window, above the dockspace.
        ///
        /// Default is a no-op; a subclass overrides it for a document toolbar.
        void OnUI() override {}

        /// @brief Returns this document's undo/redo stack, or null when the editor has none.
        ///
        /// A scene-editing document (the prefab/level editor) owns one; the single-asset settings
        /// editors have no command stack and return null. The host dispatches the Edit menu and
        /// the undo/redo shortcuts to the focused document's stack through this.
        [[nodiscard]] virtual CommandStack* GetCommandStack() { return nullptr; }

        /// @brief Writes this document's edits back to its source; the File-menu / Ctrl+S target.
        ///
        /// The host dispatches Save to the focused document through this, and it is the only path
        /// that writes the document's source. The base reports an error (no source-backed save);
        /// every concrete editor overrides it — a scene-editing document round-trips its
        /// .prefab.json, a settings editor merge-writes its JSON — then triggers the recook.
        /// @return Empty on success; an error string when the document cannot be saved this way.
        [[nodiscard]] virtual Veng::VoidResult Save()
        {
            return std::unexpected(Veng::string{"this document has no save action"});
        }

        /// @brief Returns true when the document holds edits not yet written to its source.
        ///
        /// Drives the Save action's enabled state (File menu, toolbar button) and the document
        /// title's unsaved marker. The base has no editable state and reports false; a
        /// scene-editing document reports its command stack's dirty flag (the level editor folds
        /// in its config dirtiness), and a settings editor its plain dirty flag.
        /// @return True when there is something to save.
        [[nodiscard]] virtual bool HasUnsavedChanges() const { return false; }

        /// @brief Returns true when this editor's document window or one of its children is focused.
        ///
        /// Set each Draw from ImGui window focus, including the editor's docked children, so the
        /// host can resolve which open document the keyboard shortcuts target.
        [[nodiscard]] bool IsDocumentFocused() const { return m_Focused; }

        /// @brief Returns the live Scene this document edits, or null when it edits no scene.
        ///
        /// A scene-editing document (the prefab/level editor) returns its edited Scene — the world
        /// McpHost::CurrentWorld / DocumentScene follow when this document is focused; the
        /// single-asset editors have no scene and return null (the world tools then report an
        /// empty world). Null is never a deref.
        [[nodiscard]] virtual Veng::Scene* GetDocumentScene() { return nullptr; }

        /// @brief Returns the document's rendered viewport, or null when it renders no scene.
        ///
        /// A scene-editing document returns its Offscreen scene viewport — the seam
        /// editor.screenshot_panel captures. The texture/material editors render into their own
        /// preview textures rather than a scene viewport and return null.
        [[nodiscard]] virtual Veng::Renderer::Viewport* GetDocumentViewport() { return nullptr; }

    protected:
        /// @brief Constructs the base, assigning this instance its unique dock id and class.
        AssetEditorPanel();

        /// @brief Adopts a child panel into the editor's dock area.
        /// @param child  The child panel; ownership is transferred.
        /// @return The child's index, used to dock it in BuildDefaultLayout.
        Veng::usize AddChild(Veng::Unique<EditorPanel> child);

        /// @brief Docks a child's window into a dock-builder node during layout.
        /// @param index  Child index returned by AddChild.
        /// @param node   The ImGui dock node id to dock the child into.
        void DockChildWindow(Veng::usize index, Veng::u32 node);

        /// @brief Builds the initial split layout of the child windows.
        ///
        /// Called only when the dockspace has no layout to restore — the first run, or
        /// after a layout reset — never when imgui.ini supplied one. The subclass splits
        /// @p dockspaceId into nodes and docks each child with DockChildWindow. The default
        /// is a no-op: an editor that adds no children hosts no dockspace and has no layout.
        /// @param dockspaceId  The editor's dock node id to partition.
        virtual void BuildDefaultLayout(Veng::u32 dockspaceId) { (void)dockspaceId; }

    private:
        /// @brief One docked child panel and its visibility flag.
        struct Child
        {
            /// @brief The child panel instance.
            Veng::Unique<EditorPanel> Panel;
            /// @brief Full ImGui window name ("<title>##doc<instance>"), unique across editors.
            Veng::string WindowName;
            /// @brief Whether the child window is open.
            bool Open = true;
        };

        /// @brief Submits the document window, its private dockspace, and the docked children.
        /// @param open  The host's visibility flag, forwarded to the window's close control.
        void DrawDockedWindow(bool* open);

        /// @brief Submits the single-window body of a childless editor.
        ///
        /// A childless editor hosts no dockspace: nothing could dock into it, and an empty
        /// DockSpace would eat the window's content region. Its OnUI fills the window directly.
        /// @param open  The host's visibility flag, forwarded to the window's close control.
        void DrawSingleWindow(bool* open);

        /// @brief Submits the close-with-unsaved-changes prompt and resolves the pending close.
        ///
        /// Called at top level (outside any window scope) so the popup's id stack matches the
        /// OpenPopup that armed it.
        /// @param open  The host's visibility flag, cleared once the close is confirmed.
        void ResolvePendingClose(bool* open);

        /// @brief Per-instance id disambiguating window names and the dock class across editors.
        Veng::u32 m_InstanceId;
        /// @brief ImGui id string of this editor's dockspace.
        Veng::string m_DockSpaceName;
        /// @brief The child panels, in add order.
        Veng::vector<Child> m_Children;
        /// @brief ImGui id string of this editor's close-confirmation popup.
        Veng::string m_ClosePromptName;
        /// @brief Whether this document (its window or a docked child) holds keyboard focus this frame.
        bool m_Focused = false;
        /// @brief A close was requested and is held pending the unsaved-changes prompt's answer.
        bool m_ClosePending = false;
    };
}
