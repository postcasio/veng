#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a *.font.json source into a CookedFontHeader (assetpack) plus glyph/kerning
    /// tables and an MSDF glyph atlas.
    ///
    /// The source JSON names a TTF/OTF font file (relative to the JSON's own directory), a charset,
    /// and MSDF parameters (glyph em size, pixel distance range). msdf-atlas-gen / msdfgen generate
    /// the multi-channel signed-distance field and per-glyph geometry from the font's outlines;
    /// kerning is read from the font's legacy `kern` table. The atlas is emitted as uncompressed
    /// RGBA8 — the MSDF's three channels must survive verbatim, so it is never block compressed.
    class FontImporter final : public AssetImporter
    {
    public:
        /// @brief Returns AssetType::Font.
        [[nodiscard]] AssetType Type() const override { return AssetType::Font; }

        /// @brief Cooks the font described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };
}
