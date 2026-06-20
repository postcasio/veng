#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/VertexLayout.h>

namespace Veng
{
    /// @brief AssetType::VertexLayout loader.
    ///
    /// Decodes a CookedVertexLayoutHeader + CookedVertexLayoutElement array into a
    /// Veng::VertexLayout (VertexBufferLayout), bridging the cooked underlying-integer
    /// enum fields to Veng::Renderer enums.
    class VertexLayoutLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetType::VertexLayout.
        [[nodiscard]] AssetType Type() const override { return AssetType::VertexLayout; }

        /// @brief Decodes the cooked vertex-layout blob into a LoadJob producing a resident Veng::VertexLayout.
        [[nodiscard]] AssetResult<Detail::LoadJob> Load(AssetManager& manager,
                                                        Renderer::Context& context,
                                                        TaskSystem& tasks, TypeRegistry& types,
                                                        AssetId id, std::span<const u8> cooked,
                                                        bool async) const override;
    };
}
