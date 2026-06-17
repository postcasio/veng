#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    // Cooks a texture's JSON source (e.g. "textures/brick.tex.json", referenced
    // from the pack by "source") into a CookedTextureHeader (assetpack) plus
    // raw RGBA8 pixel bytes. The source JSON's "image" path is relative to the
    // source JSON's own directory; "sampler" settings are packed into the
    // header's reserved fields. v1: stb-decoded RGBA8,
    // single mip — "generate_mips": true is unsupported — single-mip output only.
    class TextureImporter final : public AssetImporter
    {
    public:
        [[nodiscard]] AssetType Type() const override { return AssetType::Texture; }

        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context, const json& entry) const override;
    };
}
