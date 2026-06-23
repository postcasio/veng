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
        /// @brief Returns AssetType::Animation.
        [[nodiscard]] AssetType Type() const override { return AssetType::Animation; }

        /// @brief Decodes a cooked animation blob into a Ref<Animation>.
        [[nodiscard]] AssetResult<Detail::LoadJob> Load(AssetManager& manager,
                                                        Renderer::Context& context,
                                                        TaskSystem& tasks, TypeRegistry& types,
                                                        AssetId id, std::span<const u8> cooked,
                                                        bool async) const override;
    };
}
