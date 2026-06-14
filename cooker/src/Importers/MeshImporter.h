#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    // Cooks a mesh's JSON source (e.g. "meshes/cube.mesh.json", referenced from
    // the pack by "source") into a CookedMeshHeader (assetformat) plus the
    // interleaved vertex buffer, index buffer, attribute descriptor, and submesh
    // table. The source JSON's "model" path is relative to
    // the source JSON's own directory; "import" maps to assimp post-process
    // flags; "materials" assigns per-submesh material AssetIds.
    //
    // v1 flattens every assimp mesh into one buffer pair in veng's fixed
    // canonical vertex layout (position/normal/tangent/uv) with u32 indices.
    // assimp is a cooker-only dependency — it never reaches the engine.
    class MeshImporter final : public AssetImporter
    {
    public:
        [[nodiscard]] AssetType Type() const override { return AssetType::Mesh; }

        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context, const json& entry) const override;
    };
}
