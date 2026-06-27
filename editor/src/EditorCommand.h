#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>

#include "panels/PrefabEditContext.h"

namespace VengEditor
{
    /// @brief An undoable document mutation: applies a change and reverts it exactly.
    ///
    /// A command captures everything it needs to both perform and undo one edit against the
    /// live document (the PrefabEditContext's Scene + AssetManager). CommandStack drives the
    /// pair: Push() runs Apply once; Undo() runs Revert; Redo() runs Apply again — so a command
    /// must be re-runnable against the same live document any number of times. Selection is not a
    /// command (it is editor state); a command touches only document content.
    class EditorCommand
    {
    public:
        virtual ~EditorCommand() = default;

        /// @brief Performs the edit against the live document.
        /// @param ctx  The document edit context (Scene + AssetManager) to mutate.
        virtual void Apply(PrefabEditContext& ctx) = 0;

        /// @brief Reverts the edit, restoring the document to its pre-Apply state.
        /// @param ctx  The document edit context (Scene + AssetManager) to mutate.
        virtual void Revert(PrefabEditContext& ctx) = 0;

        /// @brief Returns a short human label for the Edit menu's "Undo <title>".
        [[nodiscard]] virtual Veng::string_view GetTitle() const = 0;
    };

    /// @brief Sets an entity's Transform to @p before/after; the gizmo's and the inspector's transform edit.
    ///
    /// A whole gizmo drag is one of these (start → final), so undo reverts the drag, not a frame
    /// of it. Writes back through the Scene Transform accessor so the spatial version bumps.
    class EditTransform final : public EditorCommand
    {
    public:
        /// @brief Constructs the command spanning @p entity's Transform from @p before to @p after.
        EditTransform(Veng::Entity entity, const Veng::Transform& before,
                      const Veng::Transform& after);

        void Apply(PrefabEditContext& ctx) override;
        void Revert(PrefabEditContext& ctx) override;
        [[nodiscard]] Veng::string_view GetTitle() const override { return "Move"; }

    private:
        Veng::Entity m_Entity;
        Veng::Transform m_Before;
        Veng::Transform m_After;
    };

    /// @brief Generic reflected-field edit captured as the component's serialized bytes before/after.
    ///
    /// One command covers every FieldClass without a per-field type: the inspector snapshots a
    /// component's WriteFields bytes before and after a DrawFieldWidget edit and pushes this.
    /// Apply/Revert deserialize the bytes back through the loader path (ReadFields, then
    /// RemapComponentReferences to re-resolve AssetHandle fields through the AssetManager and keep
    /// Reference fields pointing at the live entity) — never a raw memcpy, so a handle/reference
    /// field round-trips safely. A MeshRenderer re-runs ResolveEntity so its derived mesh re-streams.
    class EditField final : public EditorCommand
    {
    public:
        /// @brief Constructs the field-edit command from the before/after component byte snapshots.
        /// @param entity  The component's owning entity.
        /// @param typeId  The component's TypeId.
        /// @param before  WriteFields bytes of the component before the edit.
        /// @param after   WriteFields bytes of the component after the edit.
        EditField(Veng::Entity entity, Veng::TypeId typeId, Veng::vector<Veng::u8> before,
                  Veng::vector<Veng::u8> after);

        void Apply(PrefabEditContext& ctx) override;
        void Revert(PrefabEditContext& ctx) override;
        [[nodiscard]] Veng::string_view GetTitle() const override { return "Edit Field"; }

    private:
        /// @brief Deserializes @p bytes into the entity's component, re-resolving handles/references.
        void Restore(PrefabEditContext& ctx, const Veng::vector<Veng::u8>& bytes) const;

        Veng::Entity m_Entity;
        Veng::TypeId m_TypeId;
        Veng::vector<Veng::u8> m_Before;
        Veng::vector<Veng::u8> m_After;
    };

    /// @brief Adds a default-constructed component of @p typeId to an entity (Revert removes it).
    class AddComponentCommand final : public EditorCommand
    {
    public:
        /// @brief Constructs the add command for @p typeId on @p entity.
        AddComponentCommand(Veng::Entity entity, Veng::TypeId typeId);

        void Apply(PrefabEditContext& ctx) override;
        void Revert(PrefabEditContext& ctx) override;
        [[nodiscard]] Veng::string_view GetTitle() const override { return "Add Component"; }

    private:
        Veng::Entity m_Entity;
        Veng::TypeId m_TypeId;
    };

    /// @brief Removes a component from an entity, snapshotting its bytes so Revert restores it exactly.
    class RemoveComponentCommand final : public EditorCommand
    {
    public:
        /// @brief Constructs the remove command, snapshotting the live component's bytes.
        /// @param entity     The component's owning entity.
        /// @param typeId     The component's TypeId.
        /// @param snapshot   WriteFields bytes of the component as it stands before removal.
        RemoveComponentCommand(Veng::Entity entity, Veng::TypeId typeId,
                               Veng::vector<Veng::u8> snapshot);

        void Apply(PrefabEditContext& ctx) override;
        void Revert(PrefabEditContext& ctx) override;
        [[nodiscard]] Veng::string_view GetTitle() const override { return "Remove Component"; }

    private:
        Veng::Entity m_Entity;
        Veng::TypeId m_TypeId;
        Veng::vector<Veng::u8> m_Snapshot;
    };

    /// @brief Resets a component to its default value, snapshotting its prior bytes for Revert.
    class ResetComponentCommand final : public EditorCommand
    {
    public:
        /// @brief Constructs the reset command, snapshotting the live component's bytes.
        /// @param entity    The component's owning entity.
        /// @param typeId    The component's TypeId.
        /// @param snapshot  WriteFields bytes of the component before the reset.
        ResetComponentCommand(Veng::Entity entity, Veng::TypeId typeId,
                              Veng::vector<Veng::u8> snapshot);

        void Apply(PrefabEditContext& ctx) override;
        void Revert(PrefabEditContext& ctx) override;
        [[nodiscard]] Veng::string_view GetTitle() const override { return "Reset Component"; }

    private:
        /// @brief Deserializes @p bytes into the entity's component, re-resolving handles/references.
        void Restore(PrefabEditContext& ctx, const Veng::vector<Veng::u8>& bytes) const;

        Veng::Entity m_Entity;
        Veng::TypeId m_TypeId;
        Veng::vector<Veng::u8> m_Snapshot;
    };

    /// @brief Reparents an entity, capturing both the old and new parent + next-sibling neighbor.
    ///
    /// The intrusive Hierarchy is ordered, so reverting a move restores both parent and sibling
    /// position: each side stores the parent and the entity the moved child sat before (its
    /// NextSibling), so Apply/Revert MoveBefore back to the exact slot (or append when it was last).
    /// Covers both the explorer's reparent (drop onto a node) and reorder (drop on a strip).
    class ReparentCommand final : public EditorCommand
    {
    public:
        /// @brief Constructs the reparent command from the captured old/new neighbor.
        /// @param entity         The entity being moved.
        /// @param oldParent      Its parent before the move (Null for a root).
        /// @param oldNextSibling The sibling it sat before, or Null when it was last/only.
        /// @param newParent      Its parent after the move (Null for a root).
        /// @param newNextSibling The sibling to insert it before, or Null to append last.
        ReparentCommand(Veng::Entity entity, Veng::Entity oldParent, Veng::Entity oldNextSibling,
                        Veng::Entity newParent, Veng::Entity newNextSibling);

        void Apply(PrefabEditContext& ctx) override;
        void Revert(PrefabEditContext& ctx) override;
        [[nodiscard]] Veng::string_view GetTitle() const override { return "Reparent"; }

    private:
        /// @brief Moves m_Entity under @p parent, before @p nextSibling (append when Null).
        void MoveTo(PrefabEditContext& ctx, Veng::Entity parent, Veng::Entity nextSibling) const;

        Veng::Entity m_Entity;
        Veng::Entity m_OldParent;
        Veng::Entity m_OldNextSibling;
        Veng::Entity m_NewParent;
        Veng::Entity m_NewNextSibling;
    };

    /// @brief Creates a new entity (optionally under a parent) with a Name + Transform; Revert destroys it.
    ///
    /// Records the created handle so Revert destroys exactly it and Redo respawns the same handle
    /// (slot + generation) via Scene::CreateEntityAt — so any later stack entry capturing the
    /// entity stays valid across an undo→redo cycle.
    class CreateEntityCommand final : public EditorCommand
    {
    public:
        /// @brief Constructs the create command, optionally parenting the new entity under @p parent.
        /// @param parent  The parent for the new entity, or Null for a root.
        explicit CreateEntityCommand(Veng::Entity parent);

        void Apply(PrefabEditContext& ctx) override;
        void Revert(PrefabEditContext& ctx) override;
        [[nodiscard]] Veng::string_view GetTitle() const override { return "Create Entity"; }

        /// @brief The entity Apply created, for the caller to select. Null before the first Apply.
        [[nodiscard]] Veng::Entity GetCreated() const { return m_Created; }

    private:
        Veng::Entity m_Parent;
        Veng::Entity m_Created = Veng::Entity::Null;
    };

    /// @brief One entity's full state in a destroyed-subtree snapshot: its handle, parent, and component bytes.
    struct EntitySnapshot
    {
        /// @brief The exact entity handle (slot + generation) to recreate.
        Veng::Entity Handle = Veng::Entity::Null;
        /// @brief The entity's parent at capture time (Null for a root).
        Veng::Entity Parent = Veng::Entity::Null;
        /// @brief One captured component: its TypeId and WriteFields bytes.
        struct Component
        {
            /// @brief The component's TypeId.
            Veng::TypeId Type = Veng::InvalidTypeId;
            /// @brief WriteFields bytes of the component.
            Veng::vector<Veng::u8> Bytes;
        };
        /// @brief Every non-Hierarchy component the entity held, in capture order.
        Veng::vector<Component> Components;
    };

    /// @brief Destroys an entity and its subtree, snapshotting it so Revert respawns it exactly.
    ///
    /// The snapshot captures each entity's exact handle, parent, and component bytes in
    /// depth-first order, so Revert recreates the subtree at the same handles (Scene::CreateEntityAt)
    /// in parent-before-child order, restores every component through the loader path, rebuilds the
    /// hierarchy links, and re-resolves mesh sources. Redo destroys it again.
    class DestroyEntityCommand final : public EditorCommand
    {
    public:
        /// @brief Constructs the destroy command targeting @p root's subtree (snapshot taken on Apply).
        explicit DestroyEntityCommand(Veng::Entity root);

        void Apply(PrefabEditContext& ctx) override;
        void Revert(PrefabEditContext& ctx) override;
        [[nodiscard]] Veng::string_view GetTitle() const override { return "Delete Entity"; }

    private:
        Veng::Entity m_Root;
        /// @brief Depth-first subtree snapshot, captured on the first Apply.
        Veng::vector<EntitySnapshot> m_Snapshot;
    };

    /// @brief Duplicates an entity's subtree under its parent; Revert destroys the copy.
    ///
    /// The first Apply duplicates the source through the same component round-trip the explorer
    /// uses and records the copy's subtree handles, so Revert destroys exactly the copy and Redo
    /// respawns it at the same handles from a snapshot taken on the first Apply.
    class DuplicateEntityCommand final : public EditorCommand
    {
    public:
        /// @brief Constructs the duplicate command for @p source (its parent is read on Apply).
        explicit DuplicateEntityCommand(Veng::Entity source);

        void Apply(PrefabEditContext& ctx) override;
        void Revert(PrefabEditContext& ctx) override;
        [[nodiscard]] Veng::string_view GetTitle() const override { return "Duplicate Entity"; }

        /// @brief The duplicated root entity Apply produced, for the caller to select. Null before Apply.
        [[nodiscard]] Veng::Entity GetCopy() const { return m_Copy; }

    private:
        Veng::Entity m_Source;
        Veng::Entity m_Copy = Veng::Entity::Null;
        /// @brief Depth-first snapshot of the duplicated subtree, captured on the first Apply.
        Veng::vector<EntitySnapshot> m_Snapshot;
        /// @brief Whether the first Apply has run (subsequent Redo respawns from the snapshot).
        bool m_Captured = false;
    };
}
