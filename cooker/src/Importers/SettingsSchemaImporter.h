#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a settings-schema source (categories, settings, optional presets) into the
    /// CookedSettingsSchema blob.
    ///
    /// Serves any settings domain — a graphics `.gfxschema.json` and an audio `.audioschema.json`
    /// both cook through it, since the importer reads the manifest entry's `source` and cares not
    /// about the extension. Binds the whole schema through the shared JsonReadFields walker against
    /// the reflected SettingsSchemaData descriptor, validates it (unique ids; a discrete setting has
    /// options and a valid default; a scalar has a valid range and default; the optional default
    /// preset and every preset entry name real targets), and emits the tolerant WriteFields record
    /// so the cooker and the runtime loader share one encoder. References only engine builtins, so
    /// it needs no --module.
    class SettingsSchemaImporter : public AssetImporter
    {
    public:
        /// @brief Returns AssetTypes::SettingsSchema.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::SettingsSchema; }

        /// @brief Pure CPU over its own buffers with no shared state, so the cook may parallelize it.
        [[nodiscard]] ImporterConcurrency Concurrency() const override
        {
            return ImporterConcurrency::Parallel;
        }

        /// @brief Cooks the settings schema described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };
}
