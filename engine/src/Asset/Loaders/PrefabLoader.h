#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/Prefab.h>

// The AssetType::Prefab loader: a CookedPrefabHeader + entity/component table +
// concatenated WriteFields records -> a Veng::Prefab holding the decoded value
// tree. Embedded AssetHandle fields (a MeshRenderer's mesh, a Material, ...) are
// surfaced as ordinary LoadJob dependencies — the same machinery a Material uses
// for its textures/shaders — so they finalize before the prefab and stay
// resident for its lifetime. The prefab carries no GPU resource and needs no
// finalize beyond ordering its dependencies.

namespace Veng
{
    class PrefabLoader final : public AssetLoader
    {
    public:
        [[nodiscard]] AssetType Type() const override { return AssetType::Prefab; }

        [[nodiscard]] AssetResult<Detail::LoadJob> Load(
            AssetManager& manager, Renderer::Context& context, TaskSystem& tasks,
            TypeRegistry& types, AssetId id, std::span<const u8> cooked, bool async) const override;
    };
}
