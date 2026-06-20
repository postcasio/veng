#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/Prefab.h>

namespace Veng
{
    /// @brief AssetType::Prefab loader.
    ///
    /// Decodes a CookedPrefabHeader + entity/component table + concatenated WriteFields
    /// records into a Veng::Prefab holding the decoded value tree. Embedded AssetHandle
    /// fields are surfaced as LoadJob dependencies so they finalize before the prefab
    /// and stay resident for its lifetime. The prefab carries no GPU resource; the
    /// finalize exists solely to order dependencies before the prefab becomes resident.
    class PrefabLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetType::Prefab.
        [[nodiscard]] AssetType Type() const override { return AssetType::Prefab; }

        /// @brief Decodes the cooked prefab blob into a LoadJob producing a resident Veng::Prefab.
        [[nodiscard]] AssetResult<Detail::LoadJob> Load(AssetManager& manager,
                                                        Renderer::Context& context,
                                                        TaskSystem& tasks, TypeRegistry& types,
                                                        AssetId id, std::span<const u8> cooked,
                                                        bool async) const override;
    };
}
