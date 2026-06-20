#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a *.shader.json source into a CookedShaderHeader + reflected
    /// ShaderInterface + SPIR-V (assetpack's CookedBlobs.h).
    ///
    /// The shader JSON holds { "source": "<.slang>", "entry": "...",
    /// "vertex_layout": <AssetId, optional> }; the .slang path is resolved relative
    /// to the JSON's own directory. The named entry point is compiled with the Slang
    /// C++ API and its interface reflected via Slang's own reflection (no SPIRV-Reflect).
    /// One cooked shader covers one stage; a Material references vertex and fragment
    /// shaders as separate AssetIds. Set 0 (the bindless registry) is excluded from
    /// the reflected interface; author bindings start at set 1.
    class ShaderImporter final : public AssetImporter
    {
    public:
        /// @brief Returns AssetType::Shader.
        [[nodiscard]] AssetType Type() const override { return AssetType::Shader; }

        /// @brief Cooks the shader described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context, const json& entry) const override;
    };
}
