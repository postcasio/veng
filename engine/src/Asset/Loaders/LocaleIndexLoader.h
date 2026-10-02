#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Localization/LocaleIndex.h>

namespace Veng
{
    /// @brief AssetTypes::LocaleIndex loader: decodes a cooked index blob into a LocaleIndex.
    ///
    /// CPU-only, no dependencies, no finalize: it validates the header, reads the locale table and
    /// the endonym pool, and builds the runtime index. It never loads the catalogs it names — the
    /// localization service resolves those by id. A blob whose magic or version does not match, or
    /// whose spans run past the pool, is rejected as Corrupt.
    class LocaleIndexLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::LocaleIndex.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::LocaleIndex; }

        /// @brief Returns true: the decode runs on a worker for an asynchronous load.
        [[nodiscard]] bool ParsesOffThread() const override { return true; }

        /// @brief Decodes the cooked index blob into a resident LocaleIndex.
        [[nodiscard]] AssetResult<Detail::ParsedAsset>
        Parse(const AssetParseContext& context, AssetId id,
              std::span<const u8> cooked) const override;
    };
}
