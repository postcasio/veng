#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/Material.h>

namespace Veng
{
    /// @brief AssetTypes::Material loader.
    ///
    /// Decodes a CookedMaterialHeader + CookedMaterialField table + packed param block
    /// into a Veng::Material. Builds the graphics pipeline from the vertex/fragment
    /// shaders' reflected interface, allocates a parameter-block slot in the bindless
    /// registry, and keeps shader + texture dependencies resident for the material's lifetime.
    class MaterialLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::Material.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::Material; }

        /// @brief Decodes the cooked material blob into a LoadJob producing a resident Veng::Material.
        [[nodiscard]] AssetResult<Detail::LoadJob> Load(AssetManager& manager,
                                                        Renderer::Context& context,
                                                        TaskSystem& tasks, TypeRegistry& types,
                                                        AssetId id, std::span<const u8> cooked,
                                                        bool async) const override;
    };
}
