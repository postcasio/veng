#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a `*.loc.json` source into a CookedLocaleCatalog blob (sorted key table + pool).
    ///
    /// Parses the locale's messages through the shared ParseLocaleCatalogSource walk (so the
    /// cook-time gate and the index importer's coverage check cannot diverge) and encodes the sorted
    /// key→message table. Reads one JSON into a local buffer and validates a pure walk, so it runs
    /// ImporterConcurrency::Parallel.
    class LocaleCatalogImporter final : public AssetImporter
    {
    public:
        /// @brief Returns AssetTypes::LocaleCatalog.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::LocaleCatalog; }

        /// @brief Reentrant: reads its own source and validates a pure walk.
        [[nodiscard]] ImporterConcurrency Concurrency() const override
        {
            return ImporterConcurrency::Parallel;
        }

        /// @brief Cooks the catalog described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };
}
