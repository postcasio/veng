#pragma once

#include <Veng/Scene/Entity.h>

#include <VengEditor/EditorPanel.h>

#include "panels/PrefabEditContext.h"

namespace VengEditor
{
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
        /// @brief Constructs the explorer over the document's edit context.
        /// @param ctx  Shared document context supplying the Scene and selection.
        explicit PrefabExplorerPanel(PrefabEditContext& ctx);

        /// @brief Returns the panel title.
        [[nodiscard]] Veng::string_view GetTitle() const override { return "Hierarchy"; }

        /// @brief Builds the tree snapshot, draws it, then applies any queued mutation.
        void OnImGui() override;

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

        /// @brief Applies one queued structural op against the live scene.
        void ApplyOp(const PendingOp& op);

        /// @brief Deep-copies @p source and its subtree under @p newParent, returning the new root.
        ///
        /// Each non-Hierarchy component is round-tripped through the reflection
        /// serializer; hierarchy links are rebuilt with SetParent rather than copied.
        /// @param source     The entity to copy.
        /// @param newParent  The parent for the copied root, or Entity::Null for a root.
        /// @return The newly created copy of @p source.
        [[nodiscard]] Veng::Entity DuplicateSubtree(Veng::Entity source, Veng::Entity newParent);

        /// @brief Returns true when @p name (case-insensitive) contains the active filter substring.
        [[nodiscard]] bool MatchesFilter(Veng::string_view name) const;

        /// @brief Returns true when @p entity or any descendant matches the active filter.
        [[nodiscard]] bool SubtreeMatchesFilter(Veng::Entity entity) const;

        /// @brief Returns the display label for @p entity (its Name, or "Entity {index}").
        [[nodiscard]] Veng::string LabelOf(Veng::Entity entity) const;

        /// @brief The shared document edit context: the Scene and the selection.
        PrefabEditContext& m_Ctx;

        /// @brief Per-frame root entities (live, parent-less), rebuilt each OnImGui.
        Veng::vector<Veng::Entity> m_Roots;

        /// @brief Per-frame parent → children adjacency, rebuilt each OnImGui from the scene.
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
