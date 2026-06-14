#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    // Cooks a vertex layout asset (plan 08b) from a .vlayout.json source into a
    // CookedVertexLayoutHeader + CookedVertexLayoutElement[] blob.
    //
    // Pack entry schema:
    //   { "id": <u64>, "type": "vertex_layout", "source": "layouts/foo.vlayout.json" }
    //
    // The .vlayout.json schema:
    //   { "elements": [ { "format": "RGB32Sfloat", "name": "a_Position" }, ... ] }
    //
    // Valid format strings: R32Sfloat, RG32Sfloat, RGB32Sfloat, RGBA32Sfloat.
    class VertexLayoutImporter final : public AssetImporter
    {
    public:
        [[nodiscard]] AssetType Type() const override { return AssetType::VertexLayout; }

        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context, const json& entry) const override;
    };
}
