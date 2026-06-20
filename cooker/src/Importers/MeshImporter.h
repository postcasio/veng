#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a *.mesh.json source into a CookedMeshHeader (assetpack) plus
    /// the interleaved vertex buffer, index buffer, attribute descriptor, and submesh table.
    ///
    /// The source JSON's "model" path is relative to the source JSON's own directory;
    /// "import" maps to assimp post-process flags; "materials" assigns per-submesh
    /// material AssetIds. All assimp meshes are flattened into one buffer pair in
    /// the canonical vertex layout (position/normal/tangent/uv) with u32 indices.
    /// assimp is a cooker-only dependency — it never reaches the engine.
    class MeshImporter final : public AssetImporter
    {
    public:
        /// @brief Returns AssetType::Mesh.
        [[nodiscard]] AssetType Type() const override { return AssetType::Mesh; }

        /// @brief Cooks the mesh described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context, const json& entry) const override;
    };
}
