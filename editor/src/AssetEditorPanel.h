#pragma once

#include <Veng/Result.h>
#include <Veng/Veng.h>

#include <VengEditor/EditorPanel.h>

namespace VengEditor
{
    class CommandStack;

    /// @brief Base for an asset editor that hosts its own private dockspace.
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
        /// @brief Submits the document window, its dockspace, and the child windows.
        void Draw(bool* open) override;

        /// @brief The document body drawn inside the dockspace host window, above the dockspace.
        ///
        /// Default is a no-op; a subclass overrides it for a document toolbar.
        void OnUI() override {}

        /// @brief Returns this document's undo/redo stack, or null when the editor has none.
        ///
        /// A scene-editing document (the prefab/level editor) owns one; the texture/material
        /// editors do not derive from this base and so never offer one. The host dispatches the
        /// Edit menu and the undo/redo shortcuts to the focused document's stack through this.
        [[nodiscard]] virtual CommandStack* GetCommandStack() { return nullptr; }

        /// @brief Writes this document's edits back to its source; the File-menu / Ctrl+S target.
        ///
        /// The host dispatches Save to the focused document through this. The base reports an
        /// error (no source-backed save); a scene-editing document overrides it to round-trip its
        /// .prefab.json. The texture/material editors save through their own debounced cook loop
        /// and do not derive from this base.
        /// @return Empty on success; an error string when the document cannot be saved this way.
        [[nodiscard]] virtual Veng::VoidResult Save()
        {
            return std::unexpected(Veng::string{"this document has no save action"});
        }

        /// @brief Returns true when this editor's document window or one of its children is focused.
        ///
        /// Set each Draw from ImGui window focus, including the editor's docked children, so the
        /// host can resolve which open document the keyboard shortcuts target.
        [[nodiscard]] bool IsDocumentFocused() const { return m_Focused; }

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
        /// @p dockspaceId into nodes and docks each child with DockChildWindow.
        /// @param dockspaceId  The editor's dock node id to partition.
        virtual void BuildDefaultLayout(Veng::u32 dockspaceId) = 0;

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

        /// @brief Per-instance id disambiguating window names and the dock class across editors.
        Veng::u32 m_InstanceId;
        /// @brief ImGui id string of this editor's dockspace.
        Veng::string m_DockSpaceName;
        /// @brief The child panels, in add order.
        Veng::vector<Child> m_Children;
        /// @brief Whether this document (its window or a docked child) holds keyboard focus this frame.
        bool m_Focused = false;
    };
}
