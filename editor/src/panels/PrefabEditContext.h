#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Resolve.h>
#include <Veng/Scene/Scene.h>

#include "EditorGizmo.h"

#include <algorithm>

namespace Veng
{
    class AssetManager;
}

namespace VengEditor
{
    /// @brief Lifecycle phase of a prefab document: authoring, or running its systems.
    ///
    /// Editing edits the authored scene; Playing/Paused run the registered SceneSystems
    /// over a throwaway clone, so the authored scene is never mutated by play.
    enum class PlayState
    {
        /// @brief Authoring the scene; no systems run.
        Editing,
        /// @brief Running the systems over the play clone, advancing each frame.
        Playing,
        /// @brief Holding the play clone without advancing it.
        Paused,
    };

    /// @brief Shared editing state of one open prefab document.
    ///
    /// Owned by the PrefabEditorPanel and referenced by its child panels: the explorer
    /// mutates the selection, the inspector edits the active entity, and the viewport
    /// reads the live Scene — all over the one Scene the document spawned. The references
    /// the children hold stay valid for the document's lifetime (Scene is a stable Unique
    /// the document owns).
    struct PrefabEditContext
    {
        /// @brief Drag-drop payload tag for an Entity dragged out of the hierarchy.
        ///
        /// The explorer sets a payload of this type carrying an Entity; the explorer's
        /// reparent drops and the inspector's Reference-field drop target accept it.
        static constexpr Veng::string_view EntityPayload = "VENG_ENTITY";

        /// @brief The scene the document spawned the prefab into.
        Veng::Scene* Scene = nullptr;

        /// @brief The asset manager derived component resources resolve through.
        ///
        /// Reached by ResolveEntity so any panel sharing this context can re-run a
        /// resolver-bearing component's resolve after adding or editing it.
        Veng::AssetManager* Assets = nullptr;

        /// @brief The selected entities, in the order they were added to the selection.
        ///
        /// Empty when nothing is selected. Multi-select (Ctrl-click) targets bulk
        /// hierarchy operations (delete, reparent); the inspector edits Active.
        Veng::vector<Veng::Entity> Selection;

        /// @brief The entity the inspector edits — the last one clicked, or Null.
        Veng::Entity Active = Veng::Entity::Null;

        /// @brief The active manipulation-gizmo mode (Translate / Rotate / Scale).
        ///
        /// Document-level tool state, not per-viewport: the document toolbar and the viewports'
        /// W/E/R keys write it, and every viewport's EditorGizmo reads it, so the mode is shared
        /// across all viewports of the document.
        GizmoMode Gizmo = GizmoMode::Translate;

        /// @brief The document's current play phase.
        ///
        /// Editing while authoring; Playing/Paused while the document runs its systems
        /// over the play clone. Scene repoints to the play clone for the duration of a
        /// play session and back to the edit scene on Stop, so every child panel follows
        /// the active scene through this one pointer.
        PlayState Play = PlayState::Editing;

        /// @brief Returns true while a play session is active (Playing or Paused).
        [[nodiscard]] bool IsPlaying() const { return Play != PlayState::Editing; }

        /// @brief Returns true if @p entity is in the current selection.
        [[nodiscard]] bool IsSelected(Veng::Entity entity) const
        {
            return std::ranges::find(Selection, entity) != Selection.end();
        }

        /// @brief Returns true if anything is selected.
        [[nodiscard]] bool HasSelection() const { return !Selection.empty(); }

        /// @brief Replaces the selection with @p entity and makes it active.
        void SelectOnly(Veng::Entity entity)
        {
            Selection.clear();
            Selection.push_back(entity);
            Active = entity;
        }

        /// @brief Adds @p entity to the selection (if absent) and makes it active.
        void AddToSelection(Veng::Entity entity)
        {
            if (!IsSelected(entity))
            {
                Selection.push_back(entity);
            }
            Active = entity;
        }

        /// @brief Toggles @p entity in the selection; updates Active to a remaining member.
        void Toggle(Veng::Entity entity)
        {
            if (IsSelected(entity))
            {
                std::erase(Selection, entity);
                Active = Selection.empty() ? Veng::Entity::Null : Selection.back();
            }
            else
            {
                Selection.push_back(entity);
                Active = entity;
            }
        }

        /// @brief Rebuilds @p entity's MeshRenderer mesh from its inline recipe source.
        ///
        /// Any editor path that adds or edits a MeshRenderer carrying a recipe Source (a
        /// shape and its parameters) calls this on the touched entity so its derived mesh
        /// streams in through the ordinary async load path; there is no per-frame scan
        /// that would otherwise catch the change. An empty Source leaves the cooked Mesh
        /// untouched.
        /// @param entity  The entity whose MeshRenderer is re-resolved; must be alive.
        void ResolveEntity(Veng::Entity entity)
        {
            if (Scene == nullptr || Assets == nullptr || !Scene->IsAlive(entity))
            {
                return;
            }
            auto* renderer = Scene->TryGet<Veng::MeshRenderer>(entity);
            if (renderer != nullptr && renderer->Source.ActiveType() != Veng::InvalidTypeId)
            {
                renderer->Mesh = Veng::BuildPrimitiveMesh(*Assets, renderer->Source);
            }
        }

        /// @brief Clears the selection and the active entity.
        void Clear()
        {
            Selection.clear();
            Active = Veng::Entity::Null;
        }

        /// @brief Drops any selected entity that is no longer alive (e.g. destroyed this frame).
        ///
        /// Call once per frame before drawing: a structural edit can leave a stale handle
        /// in the selection, and every accessor asserts on a dead entity.
        void Prune()
        {
            if (Scene == nullptr)
            {
                return;
            }
            std::erase_if(Selection,
                          [this](Veng::Entity entity) { return !Scene->IsAlive(entity); });
            if (!Active.IsNull() && !Scene->IsAlive(Active))
            {
                Active = Selection.empty() ? Veng::Entity::Null : Selection.back();
            }
        }
    };
}
