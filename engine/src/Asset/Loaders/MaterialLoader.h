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

        /// @brief Returns true: the decode runs on a worker for an asynchronous load.
        [[nodiscard]] bool ParsesOffThread() const override { return true; }

        /// @brief Decodes the cooked material, naming its two shaders and its textures as
        ///        dependencies; the pipeline is built in the main-thread finalize.
        [[nodiscard]] AssetResult<Detail::ParsedAsset>
        Parse(const AssetParseContext& context, AssetId id,
              std::span<const u8> cooked) const override;
    };
}
