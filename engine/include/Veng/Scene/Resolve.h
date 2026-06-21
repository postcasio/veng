#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Scene/Components.h>

namespace Veng
{
    class Scene;
    class AssetManager;
    class Mesh;
    struct Entity;
    struct MeshData;

    /// @brief Fires every resolver-bearing component on `entity`, generating/assigning
    ///        its derived resources.
    ///
    /// Enumerates the entity's components and calls each non-null
    /// TypeInfo::SpawnResolve. The single resolve code path: Prefab::SpawnInto runs the
    /// same firing per spawned entity, and a runtime caller (the editor's add/edit, game
    /// code) calls this directly after adding or editing a component that carries a
    /// resolver. In effect idempotent for an unchanged recipe — it rebuilds and reassigns.
    /// @param scene    The scene holding the entity.
    /// @param entity   The entity whose components are resolved; must be alive.
    /// @param manager  The asset manager a resolver reaches derived resources through.
    void ResolveComponents(Scene& scene, Entity entity, AssetManager& manager);

    /// @brief Builds the CPU geometry for a primitive shape variant's active alternative.
    ///
    /// Reads the active alternative through the variant ops and calls the matching
    /// Primitives:: generator. Pure CPU — no render Context.
    /// @param shape  The shape recipe; its active alternative selects the generator.
    /// @return The generated MeshData, or nullopt when the variant is empty.
    [[nodiscard]] optional<MeshData> BuildShapeMeshData(const PrimitiveShapeVariant& shape);

    /// @brief Builds the active shape into a streaming Mesh and returns its pending handle.
    ///
    /// Generates the CPU geometry (BuildShapeMeshData) and uploads it through the async
    /// AssetManager::Build path; the handle is !IsLoaded() until the build lands a few frames
    /// later, exactly as a cooked-mesh Load. Returns an empty handle for an empty variant.
    /// Does not dedup — identical recipes build independent meshes; share the returned
    /// handle to reuse one.
    /// @param manager  The asset manager the async mesh streams through.
    /// @param shape    The shape recipe; its active alternative selects the geometry.
    /// @return The pending mesh handle, or an empty handle for an empty variant.
    [[nodiscard]] AssetHandle<Mesh> BuildPrimitiveMesh(AssetManager& manager,
                                                       const PrimitiveShapeVariant& shape);
}
