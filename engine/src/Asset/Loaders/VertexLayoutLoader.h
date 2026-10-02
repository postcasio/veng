#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/VertexLayout.h>

namespace Veng
{
    /// @brief AssetTypes::VertexLayout loader.
    ///
    /// Decodes a CookedVertexLayoutHeader + CookedVertexLayoutElement array into a
    /// Veng::VertexLayout (VertexBufferLayout), bridging the cooked underlying-integer
    /// enum fields to Veng::Renderer enums.
    class VertexLayoutLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::VertexLayout.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::VertexLayout; }

        /// @brief Returns true: the decode runs on a worker for an asynchronous load.
        [[nodiscard]] bool ParsesOffThread() const override { return true; }

        /// @brief Decodes the cooked vertex-layout blob into a resident Veng::VertexLayout.
        [[nodiscard]] AssetResult<Detail::ParsedAsset>
        Parse(const AssetParseContext& context, AssetId id,
              std::span<const u8> cooked) const override;
    };
}
