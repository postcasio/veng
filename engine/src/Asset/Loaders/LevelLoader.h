#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/Level.h>

namespace Veng
{
    /// @brief AssetTypes::Level loader.
    ///
    /// Decodes a CookedLevelHeader + system-id array + the two tolerant WriteFields records
    /// (game-mode config, render settings) into a Veng::Level. Resolves the world prefab and
    /// the game-mode config's embedded player prefab as LoadJob dependencies so they finalize
    /// before the level and stay resident for its lifetime. The level carries no GPU resource;
    /// the finalize exists solely to order dependencies before the level becomes resident.
    class LevelLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::Level.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::Level; }

        /// @brief Decodes the cooked level blob into a LoadJob producing a resident Veng::Level.
        [[nodiscard]] AssetResult<Detail::LoadJob> Load(AssetManager& manager,
                                                        Renderer::Context& context,
                                                        TaskSystem& tasks, TypeRegistry& types,
                                                        AssetId id, std::span<const u8> cooked,
                                                        bool async) const override;
    };
}
