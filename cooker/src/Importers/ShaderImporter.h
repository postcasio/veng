#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    // Cooks a shader into a CookedShaderHeader + reflected
    // ShaderInterface + SPIR-V (assetpack's CookedBlobs.h). The pack entry's
    // "source" points at a *.shader.json holding { "source": "<.slang>",
    // "entry": "...", "vertex_layout": <AssetId, optional> }; the .slang path is
    // resolved relative to that json's own directory. The named entry point is
    // compiled with the Slang C++ API and its interface reflected via Slang's
    // own reflection (no SPIRV-Reflect).
    // One cooked shader is one SPIR-V module covering one shader stage; a
    // Material asset references a vertex- and a fragment-stage shader as separate
    // AssetIds. Set 0 (the bindless registry) is recognized and excluded from the
    // reflected/validated interface.
    class ShaderImporter final : public AssetImporter
    {
    public:
        [[nodiscard]] AssetType Type() const override { return AssetType::Shader; }

        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context, const json& entry) const override;
    };
}
