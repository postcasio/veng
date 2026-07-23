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
        /// @brief Returns AssetTypes::Texture.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::Texture; }

        /// @brief Runs concurrently: the decode, resize and block-encode paths are all reentrant.
        ///
        /// stb_image keeps its failure reason thread-local and stb_image_resize2 holds only
        /// read-only tables. The block encoders each build process-global tables once, which is the
        /// one hazard: astcenc's setup calls run under a mutex and the BC7/BC4/BC5 table fills under
        /// a call_once, so the fill happens once and orders ahead of every encode that reads it. The
        /// encoders' own threading obeys CookContext::ThreadBudget rather than opening a second pool.
        [[nodiscard]] ImporterConcurrency Concurrency() const override
        {
            return ImporterConcurrency::Parallel;
        }

        /// @brief Cooks the texture described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };
}
