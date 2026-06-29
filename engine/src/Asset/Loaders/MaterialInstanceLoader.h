#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/MaterialInstance.h>

#include "MaterialLoader.h"

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
    /// A bare parent Material id used where an instance is expected resolves to the parent's
    /// implicit zero-override default instance (LoadDefaultInstance), so existing material-id
    /// references keep loading.
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

        /// @brief Builds a zero-override default instance over a parent Material blob.
        ///
        /// The default-instance rule: an AssetHandle<MaterialInstance> referencing a bare Material
        /// id resolves here. The parent Material is built inline from its own cooked blob and owned
        /// by the instance (never separately id-cached, since the instance occupies the id slot),
        /// so two default instances of the same parent id dedup to one cached instance.
        /// @param manager  The owning AssetManager.
        /// @param context  The render context.
        /// @param tasks    The task system.
        /// @param types    The engine TypeRegistry.
        /// @param id       The parent material's AssetId (also the instance's cache id).
        /// @param cooked   The cooked **Material** blob bytes.
        /// @param async    True for the async path, false for the blocking path.
        [[nodiscard]] AssetResult<Detail::LoadJob>
        LoadDefaultInstance(AssetManager& manager, Renderer::Context& context, TaskSystem& tasks,
                            TypeRegistry& types, AssetId id, std::span<const u8> cooked,
                            bool async) const;

    private:
        /// @brief The delegate that builds a parent Material from its cooked blob for the default-instance path.
        MaterialLoader m_MaterialLoader;
    };
}
