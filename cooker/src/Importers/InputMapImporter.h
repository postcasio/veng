#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a *.inputmap.json source into a CookedInputMapHeader plus a reflection record.
    ///
    /// The source declares the context's actions (id + name + kind) and a set of raw-source →
    /// action bindings. The importer validates every binding against the declared actions — an
    /// unknown action id, an axis/kind mismatch, a null or duplicate id — the way the material
    /// importer validates `.vmat` fields against a shader's reflected parameters. Enum fields
    /// (device, kind, axis) parse by name through the shared reflection enum tables. It emits the
    /// { actions, bindings } record through libveng's WriteFields, so the cook and the runtime
    /// loader share one encoder; it needs no game module (the context references only action ids).
    class InputMapImporter final : public AssetImporter
    {
    public:
        /// @brief Returns AssetTypes::InputMap.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::InputMap; }

        /// @brief Cooks the input map described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };
}
