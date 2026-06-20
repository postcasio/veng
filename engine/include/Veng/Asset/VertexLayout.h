#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Renderer/VertexBufferLayout.h>

namespace Veng
{
    /// @brief Asset wrapper around a Renderer::VertexBufferLayout.
    ///
    /// The engine ships built-in layouts (Canonical, ScreenSpace, PositionOnly) in the embedded
    /// core pack; consumers add more as assets in their own packs. A shader references its
    /// layout by AssetId (ShaderInterface::VertexLayoutId).
    struct VertexLayout
    {
        /// @brief The vertex attribute layout.
        Renderer::VertexBufferLayout Layout =
            Renderer::VertexBufferLayout(vector<Renderer::VertexBufferElement>{});

        /// @brief Returns the vertex attribute layout.
        [[nodiscard]] const Renderer::VertexBufferLayout& GetLayout() const { return Layout; }
    };

    /// @brief AssetTypeTrait specialization mapping VertexLayout to AssetType::VertexLayout.
    template <>
    struct AssetTypeTrait<VertexLayout>
    {
        /// @brief The asset type tag for VertexLayout.
        static constexpr AssetType Type = AssetType::VertexLayout;
    };
}
