#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a flipbook-atlas manifest (schema v1, `*.atlas.json`) and its image into a
    ///        CookedFlipbookHeader plus an embedded cooked texture.
    ///
    /// The pack entry's "source" names the manifest itself, so an atlas exported by a sprite-sheet
    /// tool is cooked as it stands with no wrapper file. The importer reads the core fields — the
    /// grid (`columns`, `rows`, `frames`, `frame`), the timing (`fps`, `loop`), the recommended
    /// `blend` and `alpha` mode, and `colorSpace` — plus the optional `worldExtent` and `pivot`, and
    /// ignores every other field (`source`, `technique`, `name`, `provenance`, ...). The image named
    /// by `atlas.file`, relative to the manifest, is cooked as a mipped texture at the Color role
    /// for an sRGB atlas and the Packed role for a linear one, with a linear clamp-to-edge sampler.
    ///
    /// A frame-sequence export — a manifest with no `atlas.file` — is refused, as is a manifest
    /// whose version, type, grid, or enumerated fields do not match the schema, and an image whose
    /// size is not the manifest's grid.
    class FlipbookImporter final : public AssetImporter
    {
    public:
        /// @brief Returns AssetTypes::Flipbook.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::Flipbook; }

        /// @brief Runs concurrently: the manifest parse is local and the texture cook is reentrant.
        [[nodiscard]] ImporterConcurrency Concurrency() const override
        {
            return ImporterConcurrency::Parallel;
        }

        /// @brief Cooks the flipbook described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };
}
