#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a flipbook-atlas manifest (`*.atlas.json`) and the images it names into a
    ///        CookedFlipbookHeader plus its embedded cooked textures.
    ///
    /// The pack entry's "source" names the manifest itself, so a sprite sheet exported by a tool
    /// is cooked as it stands with no wrapper file. The format, both schema versions and every
    /// refusal are documented in `cooker/FLIPBOOK_ATLAS_SCHEMA.md`.
    ///
    /// A schema-1 manifest names one `atlas` image with a declared `alpha` convention and
    /// `colorSpace`. A schema-2 manifest names a `shading` mode and the `textures` sharing its grid:
    /// an unlit premultiplied `color` sheet, or the six-way lightmaps `lightA` and `lightB` (whose
    /// alpha carries a temperature), either with an optional `motion` sheet; a six-way manifest may
    /// add a default emission ramp. Each image cooks through the ordinary texture path, mipped with
    /// a linear clamp-to-edge sampler, at the role its data needs: Color for an sRGB sheet, Packed
    /// for a linear one, Vector for motion, and Mask for the temperature, which cooks as a
    /// single-channel texture of its own.
    ///
    /// The grid (`columns`, `rows`, `frames`, `frame`), the timing (`fps`, `loop`), the recommended
    /// `blend`, and the optional `worldExtent` and `pivot` are read from either version; every other
    /// top-level field a manifest carries (`source`, `technique`, `name`, `provenance`, ...) is
    /// ignored. A frame-sequence export — a schema-1 manifest with no `atlas.file` — is refused.
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
