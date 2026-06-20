#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a *.prefab.json source (entities + components + field values) into
    /// the CookedPrefab blob.
    ///
    /// Requires CookContext::Types (the module's reflected TypeRegistry) to resolve
    /// component keys to TypeIds and validate each field against the component's
    /// reflected descriptors; absent → "requires --module" error. Uses libveng's
    /// WriteFields to emit each component's record so the cooker and the runtime
    /// loader share one encoder.
    class PrefabImporter : public AssetImporter
    {
    public:
        /// @brief Returns AssetType::Prefab.
        [[nodiscard]] AssetType Type() const override { return AssetType::Prefab; }

        /// @brief Cooks the prefab described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };
}
