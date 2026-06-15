#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    // Cooks a *.prefab.json source (entities + components + field values) into the
    // CookedPrefab blob. Unlike the other importers it needs the loaded module's
    // reflected TypeRegistry (CookContext::Types) to resolve component keys to
    // TypeIds and validate each field against the component's reflected
    // descriptors; absent → the "requires --module" error. It reuses libveng's
    // WriteFields to emit each component's record, so the cooker and the runtime
    // loader share one encoder.
    class PrefabImporter : public AssetImporter
    {
    public:
        [[nodiscard]] AssetType Type() const override { return AssetType::Prefab; }

        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context, const json& entry) const override;
    };
}
