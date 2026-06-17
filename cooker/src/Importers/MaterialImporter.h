#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    // Cooks a material into a CookedMaterialHeader + CookedMaterialField table +
    // an engine block + an authored block (assetformat's CookedBlobs.h). The pack
    // entry's "source" points at a *.vmat.json that declares:
    //   - "shaders": the vertex + fragment Shader pack entries by AssetId.
    //   - "fields": an ordered, explicitly-typed list. Each field carries a
    //     "type" (texture/sampler/float/vec2/vec3/vec4/uint): texture → Kind 1
    //     handle (an "id"); sampler → Kind 2 handle reusing a named texture
    //     field's id; both validate against and patch the engine block.
    //     scalar/vector → Kind 0 value packed into the authored block.
    // The engine block's layout is reflected from the fragment shader's
    // MaterialData struct; the authored block's from its MaterialParams struct
    // (optional — a handles-only material omits it). Both are located via the
    // shader's *.shader.json and reflected by SlangReflect::ReflectStructLayout;
    // a declared field is validated against the struct of its block by name, type,
    // and offset. One CookedMaterialField is emitted per declared field.
    class MaterialImporter final : public AssetImporter
    {
    public:
        [[nodiscard]] AssetType Type() const override { return AssetType::Material; }

        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context, const json& entry) const override;
    };
}
