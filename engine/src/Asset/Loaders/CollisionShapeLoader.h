#pragma once

#include <Veng/Asset/AssetLoader.h>

namespace Veng
{
    /// @brief Loads a CookedCollisionShapeHeader blob into a CPU-only CollisionShape asset.
    ///
    /// No GPU resource and no dependencies: the point cloud and index list are decoded directly
    /// into a Ref<CollisionShape>. The solver's own shape is built from them when a body binds
    /// the asset, which is what keeps a solver version bump a rebuild rather than a re-cook.
    class CollisionShapeLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::CollisionShape.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::CollisionShape; }

        /// @brief Returns true: the decode runs on a worker for an asynchronous load.
        [[nodiscard]] bool ParsesOffThread() const override { return true; }

        /// @brief Decodes a cooked collision-shape blob into a Ref<CollisionShape>.
        [[nodiscard]] AssetResult<Detail::ParsedAsset>
        Parse(const AssetParseContext& context, AssetId id,
              std::span<const u8> cooked) const override;
    };
}
