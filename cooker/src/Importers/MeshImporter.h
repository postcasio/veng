#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a *.mesh.json source into a CookedMeshHeader (assetpack) plus
    /// the interleaved vertex buffer, index buffer, attribute descriptor, submesh table, and
    /// socket table.
    ///
    /// The source JSON's "model" path is relative to the source JSON's own directory;
    /// "import" maps to assimp post-process flags; "materials" assigns per-submesh
    /// material AssetIds. All assimp meshes are flattened into one buffer pair in
    /// the canonical vertex layout (position/normal/tangent/uv) with u32 indices.
    /// Every node below the scene root that carries no mesh and no camera and is not a skin
    /// joint becomes a named socket at its root-space transform.
    /// assimp is a cooker-only dependency — it never reaches the engine.
    class MeshImporter final : public AssetImporter
    {
    public:
        /// @brief Returns AssetTypes::Mesh.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::Mesh; }

        /// @brief Cooks the mesh described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };
}
