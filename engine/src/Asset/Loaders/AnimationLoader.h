#pragma once

#include <Veng/Asset/AssetLoader.h>

namespace Veng
{
    /// @brief Loads a CookedAnimationHeader blob into a CPU-only Animation asset.
    ///
    /// No GPU resource and no dependencies: the channel/key tracks are decoded directly into a
    /// Ref<Animation>. An Animator component references an Animation through the ordinary
    /// load path.
    class AnimationLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::Animation.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::Animation; }

        /// @brief Returns true: the decode runs on a worker for an asynchronous load.
        [[nodiscard]] bool ParsesOffThread() const override { return true; }

        /// @brief Decodes a cooked animation blob into a Ref<Animation>.
        [[nodiscard]] AssetResult<Detail::ParsedAsset>
        Parse(const AssetParseContext& context, AssetId id,
              std::span<const u8> cooked) const override;
    };
}
