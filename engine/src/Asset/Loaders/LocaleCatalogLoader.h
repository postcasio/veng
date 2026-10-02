#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Localization/LocaleCatalog.h>

namespace Veng
{
    /// @brief AssetTypes::LocaleCatalog loader: decodes a cooked catalog blob into a LocaleCatalog.
    ///
    /// CPU-only, no dependencies, no finalize (the DataTable/Font loader shape without the streamed
    /// dependency): it validates the header, reads the sorted key table and string pool, and builds
    /// each entry's decoded message. A blob whose magic or version does not match, or whose spans
    /// run past the pool, is rejected as Corrupt.
    class LocaleCatalogLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::LocaleCatalog.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::LocaleCatalog; }

        /// @brief Returns true: the decode runs on a worker for an asynchronous load.
        [[nodiscard]] bool ParsesOffThread() const override { return true; }

        /// @brief Decodes the cooked catalog blob into a resident LocaleCatalog.
        [[nodiscard]] AssetResult<Detail::ParsedAsset>
        Parse(const AssetParseContext& context, AssetId id,
              std::span<const u8> cooked) const override;
    };
}
