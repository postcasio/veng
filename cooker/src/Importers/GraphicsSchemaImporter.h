#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a *.gfxschema.json source (categories, settings, presets) into the
    /// CookedGraphicsSchema blob.
    ///
    /// Binds the whole schema through the shared JsonReadFields walker against the reflected
    /// GraphicsSchemaData descriptor, validates it (unique ids; a discrete setting has options and
    /// a valid default; a scalar has a valid range and default; the default preset and every preset
    /// entry name real targets), and emits the tolerant WriteFields record so the cooker and the
    /// runtime loader share one encoder. References only engine builtins, so it needs no --module.
    class GraphicsSchemaImporter : public AssetImporter
    {
    public:
        /// @brief Returns AssetTypes::GraphicsSchema.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::GraphicsSchema; }

        /// @brief Pure CPU over its own buffers with no shared state, so the cook may parallelize it.
        [[nodiscard]] ImporterConcurrency Concurrency() const override
        {
            return ImporterConcurrency::Parallel;
        }

        /// @brief Cooks the graphics schema described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };
}
