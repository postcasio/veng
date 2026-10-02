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

        /// @brief Returns true: the decode, image creation and upload submit run on a worker.
        [[nodiscard]] bool ParsesOffThread() const override { return true; }

        /// @brief Decodes the cooked flipbook and its embedded atlas texture, whose Finalize registers it.
        [[nodiscard]] AssetResult<Detail::ParsedAsset>
        Parse(const AssetParseContext& context, AssetId id,
              std::span<const u8> cooked) const override;
    };
}
