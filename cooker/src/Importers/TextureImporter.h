#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a *.tex.json source into a CookedTextureHeader (assetpack) plus its mip
    /// chain's encoded bytes.
    ///
    /// The source JSON's "image" path is relative to the source JSON's own directory;
    /// "sampler" settings are packed into the header fields. An eight-bit source is
    /// stb-decoded to RGBA8 and block-compressed; a float source (OpenEXR, or a sixteen-bit
    /// image) is decoded to f32 and packed as half-float texels. "generate_mips": false opts
    /// out of the chain to a single level.
    class TextureImporter final : public AssetImporter
    {
    public:
        /// @brief Returns AssetTypes::Texture.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::Texture; }

        /// @brief Runs concurrently: the decode, resize and encode paths are all reentrant.
        ///
        /// stb_image keeps its failure reason thread-local and stb_image_resize2 holds only
        /// read-only tables; tinyexr's decode carries no process-global mutable state and spawns
        /// no threads of its own (TINYEXR_USE_THREAD is off). The block encoders each build
        /// process-global tables once, which is the one hazard: astcenc's setup calls run under a
        /// mutex and the BC7/BC4/BC5 table fills under a call_once, so the fill happens once and
        /// orders ahead of every encode that reads it. The encoders' own threading obeys
        /// CookContext::ThreadBudget rather than opening a second pool.
        [[nodiscard]] ImporterConcurrency Concurrency() const override
        {
            return ImporterConcurrency::Parallel;
        }

        /// @brief Cooks the texture described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };

    /// @brief Cooks a texture descriptor held in memory into a texture blob.
    ///
    /// The body of TextureImporter::Cook, for an importer that embeds a texture it describes
    /// itself rather than one a *.tex.json on disk names. @p descriptor carries the *.tex.json
    /// keys ("image", "role", "srgb", "generate_mips", "sampler", ...).
    /// @param context         The cook context (configuration, thread budget, dependency recorder).
    /// @param descriptor      The texture descriptor object.
    /// @param descriptorPath  The path the descriptor stands for: its directory resolves "image",
    ///                        and it names the texture in every error.
    /// @return The cooked texture blob (CookedTextureHeader plus mips), or a located error.
    [[nodiscard]] Result<vector<u8>> CookTextureDescriptor(const CookContext& context,
                                                           const json& descriptor,
                                                           const path& descriptorPath);
}
