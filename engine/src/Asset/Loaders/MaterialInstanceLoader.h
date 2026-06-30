#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/MaterialInstance.h>

namespace Veng
{
    /// @brief AssetType::MaterialInstance loader.
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
        /// @brief Returns AssetType::MaterialInstance.
        [[nodiscard]] AssetType Type() const override { return AssetType::MaterialInstance; }

        /// @brief Decodes a cooked material-instance blob into a LoadJob producing a resident MaterialInstance.
        [[nodiscard]] AssetResult<Detail::LoadJob> Load(AssetManager& manager,
                                                        Renderer::Context& context,
                                                        TaskSystem& tasks, TypeRegistry& types,
                                                        AssetId id, std::span<const u8> cooked,
                                                        bool async) const override;
    };
}
