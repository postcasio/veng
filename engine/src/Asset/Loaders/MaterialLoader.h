#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/Material.h>

// The AssetType::Material loader: a CookedMaterialHeader +
// CookedMaterialField table + packed param block -> a Veng::Material, which
// owns its forward graphics pipeline (built here from the vertex/fragment
// shaders' reflected interface), its parameter-block slot in the bindless
// registry, and keeps shader + texture dependencies resident for its lifetime.

namespace Veng
{
    class MaterialLoader final : public AssetLoader
    {
    public:
        [[nodiscard]] AssetType Type() const override { return AssetType::Material; }

        [[nodiscard]] AssetResult<Detail::LoadJob> Load(
            AssetManager& manager, Renderer::Context& context, TaskSystem& tasks,
            TypeRegistry& types, AssetId id, std::span<const u8> cooked, bool async) const override;
    };
}
