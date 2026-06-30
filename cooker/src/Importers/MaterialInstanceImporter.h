#pragma once

#include <string_view>

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

    /// @brief Cooks a parsed material-instance document (parent + sparse overrides) into a blob.
    ///
    /// The shared core of the instance cook: it resolves the parent Material, reflects its exposed
    /// field set, validates each override against it, and assembles the CookedMaterialInstance blob.
    /// The file-sourced importer reads a `*.vmatinst.json` into `inst` and calls this;
    /// CookDefaultInstanceBlob feeds it a synthesized zero-override document.
    /// @param context  The cook context (resolver + shader include dir).
    /// @param inst     The parsed instance document: a `"parent"` AssetId and optional `"overrides"`.
    /// @param label    A source label for located error messages (the file path, or a synthetic tag).
    /// @return The cooked CookedMaterialInstance blob, or a located cook error.
    [[nodiscard]] Result<vector<u8>> CookMaterialInstanceDocument(const CookContext& context,
                                                                  const json& inst,
                                                                  std::string_view label);

    /// @brief Cooks a zero-override default MaterialInstance blob over a parent Material.
    ///
    /// Synthesizes the `{ "parent": <parentId>, "overrides": {} }` document the cook emits beside a
    /// parent Material that declares a `defaultInstance` id and runs it through
    /// CookMaterialInstanceDocument, so the companion default instance is byte-identical to a
    /// hand-authored zero-override `*.vmatinst.json` over the same parent.
    /// @param context   The cook context (resolver + shader include dir).
    /// @param parentId  The parent Material's AssetId value.
    /// @return The cooked CookedMaterialInstance blob, or a located cook error.
    [[nodiscard]] Result<vector<u8>> CookDefaultInstanceBlob(const CookContext& context,
                                                             u64 parentId);
}
