#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a *.vlayout.json source into a CookedVertexLayoutHeader +
    /// CookedVertexLayoutElement[] blob.
    ///
    /// The .vlayout.json schema: `{ "elements": [ { "format": "RGB32Sfloat",
    /// "name": "a_Position" }, ... ] }`. Valid format strings: R32Sfloat,
    /// RG32Sfloat, RGB32Sfloat, RGBA32Sfloat.
    class VertexLayoutImporter final : public AssetImporter
    {
    public:
        /// @brief Returns AssetType::VertexLayout.
        [[nodiscard]] AssetType Type() const override { return AssetType::VertexLayout; }

        /// @brief Cooks the vertex layout described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };
}
