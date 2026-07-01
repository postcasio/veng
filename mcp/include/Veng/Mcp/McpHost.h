#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    class TypeRegistry;
    class AssetManager;
    class Scene;

    namespace Renderer
    {
        class Viewport;
    }
}

namespace Veng::Mcp
{
    /// @brief The kind of scene edit an McpMutation describes.
    enum class McpMutationKind : u8
    {
        /// @brief Add a component of Component to Target, seeded from Values.
        AddComponent,
        /// @brief Remove the component of Component from Target.
        RemoveComponent,
        /// @brief Apply the partial field update Values to Target's Component.
        SetField,
        /// @brief Create an entity (Name/Parent/Components) and record its handle.
        SpawnEntity,
        /// @brief Destroy Target and its subtree.
        DestroyEntity,
        /// @brief Load and spawn the prefab Asset, optionally under Parent.
        LoadPrefab,
    };

    /// @brief A resolved, validated description of one scene edit the mutation tools apply.
    ///
    /// The mutation tools build one of these per tools/call, having already resolved and
    /// validated the target entity, the component TypeId, and any asset id. A game host
    /// applies it raw to the current world; an editor host (McpHost::ApplyMutation set)
    /// hands it to the CommandStack so an agent's edit undoes like a human's. The tools
    /// never branch on host kind — they consult the hook and fall back to raw.
    ///
    /// Which fields are meaningful depends on Kind; the tool fills exactly the ones its
    /// verb needs and the handler (raw or routed) reads only those.
    struct McpMutation
    {
        /// @brief Which edit this describes.
        McpMutationKind Kind = McpMutationKind::SetField;

        /// @brief The entity the edit targets (add/remove/set-field/destroy); the parent for spawn/load-prefab.
        Entity Target = Entity::Null;

        /// @brief The component type (add/remove/set-field); InvalidTypeId when the verb has none.
        TypeId Component = InvalidTypeId;

        /// @brief The field values to apply, as a JSON object string (add/set-field/spawn).
        ///
        /// A partial update keyed by field serialization name; empty when the verb carries
        /// no values. Kept as a JSON string so this header stays JSON-library-free.
        string Values;

        /// @brief The prefab asset to load (load-prefab); an invalid id otherwise.
        AssetId Asset;

        /// @brief The name for a spawned entity (spawn); empty when unnamed.
        string Name;

        /// @brief The per-component values for a spawned entity, as a JSON object string (spawn).
        ///
        /// Keyed by component QualifiedName, each value the component's partial field object.
        /// Empty when the spawn names no components.
        string Components;
    };

    /// @brief The provider seam the app fills so the built-in tools reach live state.
    ///
    /// Mirrors VengModuleHost: the app hands the server the systems it wants reachable —
    /// references it owns and provider closures resolving per-frame state. The built-in
    /// tools capture the host by reference, so the host must outlive the server (the app
    /// owns both). Every accessor runs on the render thread during McpServer::Pump(), so a
    /// closure may freely touch engine state.
    ///
    /// A game fills CurrentWorld with its managed world and Viewport with the primary
    /// viewport; the editor fills them from the active document's scene and its panel
    /// viewports. A null CurrentWorld() (no world loaded, a closed document) makes the world
    /// tools return an empty result, never a null deref.
    struct McpHost
    {
        /// @brief Resolves a component's TypeId to its reflected fields.
        TypeRegistry& Types;

        /// @brief The asset system, for asset queries and id → name lookups.
        AssetManager& Assets;

        /// @brief Returns the scene an agent inspects, or null when no world is loaded.
        ///
        /// Called on the render thread at each world tool. A null return means "no world";
        /// every world tool handles it by returning an empty/so-stated result.
        function<Scene*()> CurrentWorld;

        /// @brief Resolves a viewport by name, or null when unknown or unset.
        ///
        /// The render tools (render.screenshot, render.stats) resolve a viewport through
        /// this. A game fills it to return its primary viewport for a well-known name
        /// (e.g. "" or "primary") and null otherwise; the editor returns a named panel's
        /// viewport. Left unset (or returning null) makes the render tools report "no
        /// viewport", never a null deref.
        function<Renderer::Viewport*(string_view name)> Viewport;

        /// @brief Names the viewports the render tools expose, or empty when none.
        ///
        /// render.list_viewports reports these names; the app both names them here and
        /// resolves each through Viewport, so the server never enumerates the engine
        /// drive-list itself. Left unset (or returning an empty list) leaves
        /// render.list_viewports reporting no viewports, not a crash.
        function<vector<string>()> ViewportNames;

        /// @brief Optional editor hook consulted before a mutation touches the scene, or null.
        ///
        /// Null in a game host: a mutation tool applies its McpMutation raw to CurrentWorld().
        /// Set by an editor host: the tool hands the McpMutation to the host, which pushes the
        /// corresponding editor command onto the CommandStack (so an agent's edit is undoable and
        /// marks the document dirty) and returns true. A return of false means the host declined
        /// to handle the edit and the tool falls back to the raw path.
        ///
        /// The hook runs on the render thread during Pump(), outside any View/Each iteration, so
        /// it may freely mutate the scene. An editor host over a document scene must set it — a
        /// forgotten wiring silently produces un-undoable agent edits, which the editor host
        /// guards against at construction (Plan 04a).
        function<bool(const McpMutation&)> ApplyMutation;
    };
}
