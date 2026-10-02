#pragma once

#include <Veng/Asset/AssetLoader.h>

namespace Veng
{
    /// @brief Loads a CookedSkeletonHeader blob into a CPU-only Skeleton asset.
    ///
    /// No GPU resource and no dependencies: the bone table is decoded directly into a
    /// Ref<Skeleton>. A skinned Mesh resolves its Skeleton through the ordinary load path.
    class SkeletonLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::Skeleton.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::Skeleton; }

        /// @brief Returns true: the decode runs on a worker for an asynchronous load.
        [[nodiscard]] bool ParsesOffThread() const override { return true; }

        /// @brief Decodes a cooked skeleton blob into a Ref<Skeleton>.
        [[nodiscard]] AssetResult<Detail::ParsedAsset>
        Parse(const AssetParseContext& context, AssetId id,
              std::span<const u8> cooked) const override;
    };
}
