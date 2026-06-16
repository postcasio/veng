#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/VertexLayout.h>

// The AssetType::VertexLayout loader: a
// CookedVertexLayoutHeader + CookedVertexLayoutElement[] -> a
// Veng::VertexLayout (a VertexBufferLayout), bridging the cooked
// underlying-integer enum fields to Veng::Renderer enums.

namespace Veng
{
    class VertexLayoutLoader final : public AssetLoader
    {
    public:
        [[nodiscard]] AssetType Type() const override { return AssetType::VertexLayout; }

        [[nodiscard]] AssetResult<Detail::LoadJob> Load(
            AssetManager& manager, Renderer::Context& context, TaskSystem& tasks,
            TypeRegistry& types, AssetId id, std::span<const u8> cooked, bool async) const override;
    };
}
