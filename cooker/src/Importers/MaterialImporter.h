#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a *.vmat.json material source into a CookedMaterialHeader +
    /// CookedMaterialField table + a single param block (assetpack's CookedBlobs.h).
    ///
    /// The source declares "domain" ("surface" or "postprocess"), "shaders" (vertex +
    /// fragment AssetIds), and "fields" (ordered, explicitly-typed list). Each field
    /// carries a "type": texture → Kind 1 handle; sampler → Kind 2 handle reusing a
    /// named texture field's id; scalar/vector → Kind 0 value. The param block layout
    /// is reflected from the fragment shader's MaterialParams struct via
    /// SlangReflect::ReflectStructLayout; declared fields are validated by name, type,
    /// and offset. The fragment entry's SV_TargetN outputs are validated against the
    /// domain contract: Surface writes float4 SV_Target0+1+2 (the g-buffer);
    /// PostProcess writes a single float4 SV_Target0.
    class MaterialImporter final : public AssetImporter
    {
    public:
        /// @brief Returns AssetType::Material.
        [[nodiscard]] AssetType Type() const override { return AssetType::Material; }

        /// @brief Cooks the material described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };
}
