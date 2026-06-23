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
        /// @brief Returns AssetType::Skeleton.
        [[nodiscard]] AssetType Type() const override { return AssetType::Skeleton; }

        /// @brief Decodes a cooked skeleton blob into a Ref<Skeleton>.
        [[nodiscard]] AssetResult<Detail::LoadJob> Load(AssetManager& manager,
                                                        Renderer::Context& context,
                                                        TaskSystem& tasks, TypeRegistry& types,
                                                        AssetId id, std::span<const u8> cooked,
                                                        bool async) const override;
    };
}
