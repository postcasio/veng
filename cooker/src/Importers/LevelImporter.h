#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a *.level.json source (world prefab + system set + game-mode + render config)
    /// into the CookedLevel blob.
    ///
    /// Requires CookContext::Types and CookContext::Systems (the module's reflected
    /// TypeRegistry + SystemRegistry) — absent → "requires --module" error. Validates the
    /// world prefab reference, that each `systems` id resolves against the catalog, and the
    /// `gameMode`/`render` config against their reflected struct descriptors, mirroring the
    /// PrefabImporter. Uses libveng's WriteFields to emit the two config records so the cooker
    /// and the runtime loader share one encoder.
    class LevelImporter : public AssetImporter
    {
    public:
        /// @brief Returns AssetTypes::Level.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::Level; }

        /// @brief Reads CookContext::Types and ::Systems, so a module rebuild can change its output.
        [[nodiscard]] ImporterModuleDependence ModuleDependence() const override
        {
            return ImporterModuleDependence::DependsOnModule;
        }

        /// @brief Cooks the level described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };
}
