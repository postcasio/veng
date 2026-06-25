#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a *.env.json source into a CookedEnvironmentHeader (assetpack) plus
    /// half-float (RGBA16Sfloat) equirectangular HDR pixels.
    ///
    /// The source JSON's "image" path (an OpenEXR panorama) is relative to the source
    /// JSON's own directory and decoded with tinyexr. An optional "max_size" downscales
    /// the larger edge (aspect-preserving, linear) before packing so a high-resolution
    /// panorama does not bloat the cooked blob. The engine generates the IBL cubemap,
    /// irradiance, and prefilter maps from this panorama at load.
    class EnvironmentImporter final : public AssetImporter
    {
    public:
        /// @brief Returns AssetType::Environment.
        [[nodiscard]] AssetType Type() const override { return AssetType::Environment; }

        /// @brief Cooks the environment described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };
}
