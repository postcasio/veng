#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Renderer/VertexBufferLayout.h>

// VertexLayoutAsset: a loaded vertex-buffer layout, a thin
// asset wrapper around Renderer::VertexBufferLayout. The engine ships built-in
// layouts (Canonical, ScreenSpace, PositionOnly) in an embedded core pack;
// consumers add more as assets in their own packs. A shader references its
// layout by AssetId (ShaderInterface::VertexLayoutId).
namespace Veng::Renderer
{
    struct VertexLayoutAsset
    {
        VertexBufferLayout Layout = VertexBufferLayout(vector<VertexBufferElement>{});
        [[nodiscard]] const VertexBufferLayout& GetLayout() const { return Layout; }
    };
}

namespace Veng
{
    template <>
    struct AssetTypeTrait<Renderer::VertexLayoutAsset>
    {
        static constexpr AssetType Type = AssetType::VertexLayout;
    };
}
