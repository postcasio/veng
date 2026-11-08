#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a *.shader.json source into a CookedShaderHeader + reflected
    /// ShaderInterface + one code section per shader target (assetpack's CookedShader.h).
    ///
    /// The shader JSON holds { "source": "<.slang>", "entry": "...",
    /// "vertex_layout": <AssetId, optional> }; the .slang path is resolved relative
    /// to the JSON's own directory. The named entry point is compiled with the Slang
    /// C++ API and its interface reflected via Slang's own reflection (no SPIRV-Reflect).
    /// One cooked shader covers one stage; a Material references vertex and fragment
    /// shaders as separate AssetIds. The bindless registries' sets (0-2) are excluded from
    /// the reflected interface; author bindings start at set 3.
    class ShaderImporter final : public AssetImporter
    {
    public:
        /// @brief Returns AssetTypes::Shader.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::Shader; }

        /// @brief Cooks the shader described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };
}
