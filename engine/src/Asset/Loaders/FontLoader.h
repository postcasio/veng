#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/Font.h>

namespace Veng
{
    /// @brief AssetTypes::Font loader.
    ///
    /// Decodes a CookedFontHeader, the face bytes, the hot-set codepoints and the fallback chain
    /// into a Veng::Font. The fallbacks are dependencies kept resident; loading the face into the
    /// shared rasterizer and warming the hot set into the shared atlas are main-thread steps, run in
    /// the Finalize once the fallbacks are resident.
    class FontLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::Font.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::Font; }

        /// @brief Returns true: the decode runs on a worker for an asynchronous load.
        [[nodiscard]] bool ParsesOffThread() const override { return true; }

        /// @brief Decodes the cooked font blob, naming its fallback fonts as dependencies.
        [[nodiscard]] AssetResult<Detail::ParsedAsset>
        Parse(const AssetParseContext& context, AssetId id,
              std::span<const u8> cooked) const override;
    };
}
