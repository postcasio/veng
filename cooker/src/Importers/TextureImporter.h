#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a *.tex.json source into a CookedTextureHeader (assetpack) plus
    /// stb-decoded RGBA8 pixel bytes (single mip).
    ///
    /// The source JSON's "image" path is relative to the source JSON's own directory;
    /// "sampler" settings are packed into the header fields. "generate_mips": true is
    /// rejected at cook time — single-mip output only.
    class TextureImporter final : public AssetImporter
    {
    public:
        /// @brief Returns AssetType::Texture.
        [[nodiscard]] AssetType Type() const override { return AssetType::Texture; }

        /// @brief Cooks the texture described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context, const json& entry) const override;
    };
}
