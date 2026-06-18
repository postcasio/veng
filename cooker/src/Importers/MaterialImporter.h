#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    // Cooks a material into a CookedMaterialHeader + CookedMaterialField table +
    // a single param block (assetpack's CookedBlobs.h). The pack entry's "source"
    // points at a *.vmat.json that declares:
    //   - "domain" (optional): "surface" (default) or "postprocess". It selects the
    //     fragment output contract the cook validates the shader against.
    //   - "shaders": the vertex + fragment Shader pack entries by AssetId.
    //   - "fields": an ordered, explicitly-typed list. Each field carries a
    //     "type" (texture/sampler/float/vec2/vec3/vec4/uint): texture → Kind 1
    //     handle (an "id"); sampler → Kind 2 handle reusing a named texture
    //     field's id; scalar/vector → Kind 0 value. All share the one block.
    // The block's layout is reflected from the fragment shader's MaterialParams
    // struct (optional — a fieldless material omits it), located via the shader's
    // *.shader.json and reflected by SlangReflect::ReflectStructLayout; a declared
    // field is validated against the struct by name, type, and offset. One
    // CookedMaterialField is emitted per declared field. The fragment entry's
    // SV_TargetN outputs are reflected and validated against the domain's contract:
    // Surface writes float4 SV_Target0 + float4 SV_Target1 (the g-buffer);
    // PostProcess writes a single float4 SV_Target0.
    class MaterialImporter final : public AssetImporter
    {
    public:
        [[nodiscard]] AssetType Type() const override { return AssetType::Material; }

        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context, const json& entry) const override;
    };
}
