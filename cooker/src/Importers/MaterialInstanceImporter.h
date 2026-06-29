#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a *.vmatinst.json material-instance source into a CookedMaterialInstanceHeader +
    /// CookedMaterialInstanceOverride table + override value region (assetpack's CookedBlobs.h).
    ///
    /// The source declares "parent" (a parent Material AssetId) and a sparse "overrides" map
    /// (parent-field name → value, or texture field → AssetId). Each override is validated
    /// against the parent's exposed reflected fields — the parent's own declared field list,
    /// validated by type and offset against the parent fragment shader's reflected MaterialParams,
    /// exactly the .vmat-against-shader check lifted one level to instance-against-parent. An
    /// override naming a field the parent does not expose, or a type mismatch, is a located cook
    /// error. An omitted field inherits the parent default (schema tolerance). The instance owns
    /// no shader or pipeline: the parent supplies those; the cook emits only the sparse override
    /// set the runtime seeds its SSBO slot with.
    class MaterialInstanceImporter final : public AssetImporter
    {
    public:
        /// @brief Returns AssetType::MaterialInstance.
        [[nodiscard]] AssetType Type() const override { return AssetType::MaterialInstance; }

        /// @brief Cooks the material instance described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };
}
