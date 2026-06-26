#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Scene/Components.h>

namespace Veng
{
    class AssetManager;
    class Mesh;
    struct MeshData;

    /// @brief Builds the CPU geometry for a mesh source's active shape alternative.
    ///
    /// Reads the active alternative through the variant ops and calls the matching
    /// Primitives:: generator. Pure CPU — no render Context.
    /// @param source  The mesh source; its active alternative selects the generator.
    /// @return The generated MeshData, or nullopt when the source is empty.
    [[nodiscard]] optional<MeshData> BuildShapeMeshData(const MeshSource& source);

    /// @brief Builds the active shape into a streaming Mesh and returns its pending handle.
    ///
    /// Generates the CPU geometry (BuildShapeMeshData) and uploads it through the async
    /// AssetManager::Build path; the handle is !IsLoaded() until the build lands a few frames
    /// later, exactly as a cooked-mesh Load. Returns an empty handle for an empty source.
    /// Does not dedup — identical recipes build independent meshes; share the returned
    /// handle to reuse one.
    /// @param manager  The asset manager the async mesh streams through.
    /// @param source   The mesh source; its active alternative selects the geometry.
    /// @return The pending mesh handle, or an empty handle for an empty source.
    [[nodiscard]] AssetHandle<Mesh> BuildPrimitiveMesh(AssetManager& manager,
                                                       const MeshSource& source);
}
