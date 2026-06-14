#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Renderer/Material.h>

// The AssetType::Material loader (planset-5 plan 09): a CookedMaterialHeader +
// CookedMaterialField table + packed param block -> a Renderer::Material, which
// owns its forward graphics pipeline (built here from the vertex/fragment
// shaders' reflected interface), its MaterialData slot in the bindless registry,
// and keeps shader + texture dependencies resident for its lifetime.

namespace Veng
{
    class MaterialLoader final : public AssetLoader
    {
    public:
        [[nodiscard]] AssetType Type() const override { return AssetType::Material; }

        [[nodiscard]] AssetResult<Detail::RefAny> Load(
            AssetManager& manager, Renderer::Context& context,
            AssetId id, std::span<const u8> cooked) const override;
    };
}
