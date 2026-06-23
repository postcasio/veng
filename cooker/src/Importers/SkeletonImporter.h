#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a *.skeleton.json source into a CookedSkeletonHeader plus a bone table.
    ///
    /// The source JSON's "model" path (relative to the source JSON's directory) names the
    /// rigged model file; the importer flattens its assimp node hierarchy into the canonical
    /// bone order (shared with the MeshImporter and AnimationImporter via SkeletonSource), so
    /// a skinned mesh's bone indices, this skeleton's bones, and an animation's channels all
    /// agree. assimp is a cooker-only dependency.
    class SkeletonImporter final : public AssetImporter
    {
    public:
        /// @brief Returns AssetType::Skeleton.
        [[nodiscard]] AssetType Type() const override { return AssetType::Skeleton; }

        /// @brief Cooks the skeleton described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };
}
