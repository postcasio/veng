#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    // Cooks a material into a CookedMaterialHeader +
    // CookedMaterialField table + packed parameter block (assetformat's
    // CookedBlobs.h). The material JSON references:
    //   - Two Shader pack entries (vertex + fragment) by AssetId.
    //   - A "textures" map: MaterialData field name → texture AssetId (Kind 1
    //     handle); an adjacent "<name>Sampler" field is classified as Kind 2.
    //   - A "params" map: MaterialData field name → scalar or float-array value,
    //     packed at the reflected byte offset (Kind 0 fields).
    // MaterialData's layout is reflected from the fragment shader's .slang source
    // via SlangReflect::ReflectStructLayout — inline (spirv_b64) fragment shaders
    // are unsupported: there is no source file to reflect from.
    class MaterialImporter final : public AssetImporter
    {
    public:
        [[nodiscard]] AssetType Type() const override { return AssetType::Material; }

        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context, const json& entry) const override;
    };
}
