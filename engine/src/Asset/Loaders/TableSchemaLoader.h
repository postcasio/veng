#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/DataTable.h>

namespace Veng
{
    /// @brief AssetTypes::TableSchema loader: decodes the cooked column table into a TableSchema.
    ///
    /// A CPU-only asset with no dependencies; a truncated or version-mismatched blob is rejected
    /// as AssetError::Corrupt rather than asserted on.
    class TableSchemaLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::TableSchema.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::TableSchema; }

        /// @brief Decodes the cooked schema blob into a LoadJob producing a resident TableSchema.
        [[nodiscard]] AssetResult<Detail::LoadJob> Load(AssetManager& manager,
                                                        Renderer::Context& context,
                                                        TaskSystem& tasks, TypeRegistry& types,
                                                        AssetId id, std::span<const u8> cooked,
                                                        bool async) const override;
    };
}
