#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/Flipbook.h>

namespace Veng
{
    /// @brief AssetTypes::Flipbook loader.
    ///
    /// Decodes a CookedFlipbookHeader and hands the embedded cooked texture to the texture
    /// loader, so the atlas takes exactly the path — codec gate, texture-quality mip cap, async
    /// upload, main-thread bindless registration — a standalone texture takes. The flipbook's
    /// finalize step finalizes that texture, so a resident flipbook's atlas is always sampleable.
    class FlipbookLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::Flipbook.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::Flipbook; }

        /// @brief Decodes the cooked flipbook blob into a LoadJob producing a resident Flipbook.
        [[nodiscard]] AssetResult<Detail::LoadJob> Load(AssetManager& manager,
                                                        Renderer::Context& context,
                                                        TaskSystem& tasks, TypeRegistry& types,
                                                        AssetId id, std::span<const u8> cooked,
                                                        bool async) const override;
    };
}
