#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a *.animation.json source into a CookedAnimationHeader plus per-bone
    /// keyframe tracks.
    ///
    /// The source JSON's "model" path (relative to the source JSON's directory) names the
    /// animated model file; an optional "clip" selects the animation by index (default 0).
    /// Channels target bones by the canonical bone order shared with the SkeletonImporter
    /// (SkeletonSource), so an animation and its skeleton agree on bone indices. Key times are
    /// converted to seconds. assimp is a cooker-only dependency.
    class AnimationImporter final : public AssetImporter
    {
    public:
        /// @brief Returns AssetType::Animation.
        [[nodiscard]] AssetType Type() const override { return AssetType::Animation; }

        /// @brief Cooks the animation described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };
}
