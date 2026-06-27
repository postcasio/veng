#pragma once

#include <Veng/Scene/Entity.h>

#include <VengEditor/EditorPanel.h>

#include "panels/PrefabEditContext.h"

namespace VengEditor
{
    class CommandStack;

    /// @brief Entity-hierarchy explorer for the prefab editor.
    ///
    /// Walks the document's Scene as a parent/child tree and drives the shared
    /// selection: clicking an entity selects it for the inspector. Supports
    /// multi-select, inline rename, drag-and-drop reparent/reorder, a per-row and
    /// empty-space context menu, and create/duplicate/delete. Structural edits are
    /// never applied while the snapshot is being walked — they are queued during the
    /// draw and applied after it returns, so the Scene's no-structural-change-during-
    /// iteration contract holds.
    class PrefabExplorerPanel final : public EditorPanel
    {
    public:
        /// @brief Constructs the explorer over the document's edit context and command stack.
        /// @param ctx       Shared document context supplying the Scene and selection.
        /// @param commands  The document's undo/redo stack every structural edit is pushed through.
        PrefabExplorerPanel(PrefabEditContext& ctx, CommandStack& commands);

        /// @brief Returns the panel title.
        [[nodiscard]] Veng::string_view GetTitle() const override { return "Hierarchy"; }

        /// @brief Builds the tree snapshot, draws it, then applies any queued mutation.
        void OnUI() override;

    private:
        /// @brief A structural edit discovered during the draw, applied after the walk returns.
        struct PendingOp
        {
            /// @brief The kind of structural change to apply.
            enum class Kind : Veng::u32
            {
                /// @brief Create a new root entity and select it.
                CreateRoot,
                /// @brief Create a child entity under Target and select it.
                CreateChild,
                /// @brief Reparent Source under Target (SetParent).
                Reparent,
                /// @brief Move Source immediately before Target in its sibling list (MoveBefore).
                MoveBefore,
                /// @brief Detach Source to the root (Detach).
                DetachToRoot,
                /// @brief Deep-copy Source and its subtree, then select the copy.
                Duplicate,
                /// @brief Destroy every currently selected entity (recursive).
                DeleteSelection,
            };

            /// @brief Which structural change to apply.
            Kind Op = Kind::CreateRoot;
            /// @brief The entity acted on (the moved/duplicated/parented child), when the op uses one.
            Veng::Entity Source = Veng::Entity::Null;
            /// @brief The reference entity (new parent, or sibling to move before), when the op uses one.
            Veng::Entity Target = Veng::Entity::Null;
        };

        /// @brief Rebuilds the per-frame root list and parent → children adjacency from the live scene.
        void BuildSnapshot();

        /// @brief Draws @p entity and its child subtree as selectable, drag-droppable tree rows.
        void DrawEntity(Veng::Entity entity);

        /// @brief Draws the thin reorder drop strip that, on a drop, queues a MoveBefore @p before.
        /// @param before  The sibling the dropped entity is inserted ahead of.
        /// @param depth   Tree depth, used to give each strip a unique id.
        void DrawReorderTarget(Veng::Entity before, Veng::u32 depth);

        /// @brief Draws the rename input over @p entity and commits or cancels it.
        void DrawRenameField(Veng::Entity entity);

        /// @brief Records a rename of @p entity's Name as undoable command(s) from m_RenameScratch.
        ///
        /// Adds a Name component first (its own undo step) when the entity has none, so the field
        /// edit always operates on an existing component.
        /// @param entity  The entity whose Name is set.
        void CommitRename(Veng::Entity entity);

        /// @brief Builds the command for one queued structural op and pushes it onto the stack.
        void ApplyOp(const PendingOp& op);

        /// @brief Returns true when @p name (case-insensitive) contains the active filter substring.
        [[nodiscard]] bool MatchesFilter(Veng::string_view name) const;

        /// @brief Returns true when @p entity or any descendant matches the active filter.
        [[nodiscard]] bool SubtreeMatchesFilter(Veng::Entity entity) const;

        /// @brief Returns the display label for @p entity (its Name, or "Entity {index}").
        [[nodiscard]] Veng::string LabelOf(Veng::Entity entity) const;

        /// @brief The shared document edit context: the Scene and the selection.
        PrefabEditContext& m_Ctx;

        /// @brief The document's undo/redo stack every structural edit is pushed through.
        CommandStack& m_Commands;

        /// @brief Per-frame root entities (live, parent-less), rebuilt each OnUI.
        Veng::vector<Veng::Entity> m_Roots;

        /// @brief Per-frame parent → children adjacency, rebuilt each OnUI from the scene.
        Veng::unordered_map<Veng::Entity, Veng::vector<Veng::Entity>> m_Children;

        /// @brief Structural ops queued during the draw, applied after the walk returns.
        Veng::vector<PendingOp> m_Pending;

        /// @brief Raw name filter text from the toolbar search box, as the user typed it.
        Veng::string m_Filter;

        /// @brief Lowercased copy of m_Filter for case-insensitive matching; empty shows the whole tree.
        Veng::string m_FilterLower;

        /// @brief The entity currently being inline-renamed, or Entity::Null when none.
        Veng::Entity m_Renaming = Veng::Entity::Null;

        /// @brief Rename scratch buffer; committed to the entity's Name on Enter/deactivate.
        Veng::string m_RenameScratch;
    };
}
