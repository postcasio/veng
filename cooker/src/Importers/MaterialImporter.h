#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    // Cooks a material into a CookedMaterialHeader +
    // CookedMaterialField table + packed parameter block (assetformat's
    // CookedBlobs.h). The pack entry's "source" points at a *.vmat.json that
    // declares:
    //   - "shaders": the vertex + fragment Shader pack entries by AssetId.
    //   - "fields": an ordered, explicitly-typed list. Each field carries a
    //     "type" (texture/sampler/float/vec2/vec3/vec4/uint): texture → Kind 1
    //     handle (an "id"); sampler → Kind 2 handle reusing a named texture
    //     field's id; scalar/vector → Kind 0 value packed into the param block.
    // MaterialData's byte layout is reflected from the fragment shader's .slang
    // source (located via its *.shader.json) by SlangReflect::ReflectStructLayout;
    // declared fields are validated against it by name, type, and offset.
    class MaterialImporter final : public AssetImporter
    {
    public:
        [[nodiscard]] AssetType Type() const override { return AssetType::Material; }

        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context, const json& entry) const override;
    };
}
