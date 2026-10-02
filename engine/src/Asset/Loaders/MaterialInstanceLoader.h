#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/MaterialInstance.h>

namespace Veng
{
    /// @brief AssetTypes::MaterialInstance loader.
    ///
    /// Decodes a CookedMaterialInstanceHeader + override table into a Veng::MaterialInstance:
    /// resolves the parent Material as a dependency, copies its default block, applies the
    /// overrides by reflected offset, registers any override textures into bindless, and allocates
    /// the instance's own per-material SSBO slot. The pipeline, layout, and schema come from the
    /// parent — the instance builds no pipeline.
    ///
    /// A material reference names a real MaterialInstance archive entry; the cook emits a companion
    /// zero-override instance beside each parent Material that declares a `defaultInstance` id, so a
    /// direct reference to a material loads that cooked instance.
    class MaterialInstanceLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::MaterialInstance.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::MaterialInstance; }

        /// @brief Returns true: the decode runs on a worker for an asynchronous load.
        [[nodiscard]] bool ParsesOffThread() const override { return true; }

        /// @brief Decodes the cooked instance, naming its parent material and its override
        ///        textures as dependencies.
        [[nodiscard]] AssetResult<Detail::ParsedAsset>
        Parse(const AssetParseContext& context, AssetId id,
              std::span<const u8> cooked) const override;
    };
}
