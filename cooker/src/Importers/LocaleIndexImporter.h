#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a `*.locindex.json` source into a CookedLocaleIndex blob, gating the locale set.
    ///
    /// The one place that knows every locale's catalog, so it is where cross-locale validation runs:
    /// each catalog id resolves through CookContext::Resolve (a dangling reference is a cook error),
    /// the fallback graph is validated (every fallback names an indexed locale, the source's fallback
    /// is itself, every chain reaches the source without a cycle), and a translation-coverage diff of
    /// each non-source locale against the source is emitted — a warning by default, a cook error when
    /// the index declares `"coverage": "complete"`. It RecordDependencys each catalog it names so a
    /// catalog edit re-runs the coverage check. Reentrant: reads sources and validates a pure walk.
    class LocaleIndexImporter final : public AssetImporter
    {
    public:
        /// @brief Returns AssetTypes::LocaleIndex.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::LocaleIndex; }

        /// @brief Reentrant: reads its own and the catalogs' sources and validates a pure walk.
        [[nodiscard]] ImporterConcurrency Concurrency() const override
        {
            return ImporterConcurrency::Parallel;
        }

        /// @brief Cooks the index described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };
}
