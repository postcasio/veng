#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Copies the "source" file (relative to CookContext::PackDir) verbatim
    /// into an AssetTypes::Raw blob with no type-specific processing.
    class RawImporter final : public AssetImporter
    {
    public:
        /// @brief Returns AssetTypes::Raw.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::Raw; }

        /// @brief Runs concurrently: the cook is a file read into a fresh buffer, driving no library.
        [[nodiscard]] ImporterConcurrency Concurrency() const override
        {
            return ImporterConcurrency::Parallel;
        }

        /// @brief Cooks the raw asset described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };
}
