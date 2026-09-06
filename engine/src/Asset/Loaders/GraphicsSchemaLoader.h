#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Render/GraphicsSchema.h>

namespace Veng
{
    /// @brief AssetTypes::GraphicsSchema loader.
    ///
    /// Decodes a CookedGraphicsSchemaHeader + the tolerant WriteFields schema record into a
    /// Veng::GraphicsSchema. The schema carries no GPU resource and no dependencies; a
    /// stale/foreign or truncated blob surfaces as AssetError::Corrupt rather than a crash.
    class GraphicsSchemaLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::GraphicsSchema.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::GraphicsSchema; }

        /// @brief Decodes the cooked schema blob into a LoadJob producing a resident GraphicsSchema.
        [[nodiscard]] AssetResult<Detail::LoadJob> Load(AssetManager& manager,
                                                        Renderer::Context& context,
                                                        TaskSystem& tasks, TypeRegistry& types,
                                                        AssetId id, std::span<const u8> cooked,
                                                        bool async) const override;
    };
}
