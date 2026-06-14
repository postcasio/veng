#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    // Cooks a shader into a CookedShaderHeader + reflected
    // ShaderInterface + SPIR-V (assetformat's CookedBlobs.h). Two input forms:
    //   - { "type": "shader", "source": "shaders/mesh.vert.slang", "entry": "vsMain" }
    //     compiles the named entry point with the Slang C++ API and reflects its
    //     interface via Slang's own reflection (no SPIRV-Reflect).
    //   - { "type": "shader", "spirv_b64": "...", "entry": "main", "interface": {...} }
    //     precompiled SPIR-V (base64) with its ShaderInterface supplied directly
    //     (the editor/inline path) — validated and passed through unchanged.
    // Either way, one cooked shader is one SPIR-V module covering one shader
    // stage; a Material asset references a vertex- and a fragment-stage
    // shader as separate AssetIds. Set 0 (the bindless registry) is
    // recognized and excluded from the reflected/validated interface.
    class ShaderImporter final : public AssetImporter
    {
    public:
        [[nodiscard]] AssetType Type() const override { return AssetType::Shader; }

        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context, const json& entry) const override;
    };
}
