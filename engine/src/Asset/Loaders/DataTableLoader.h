#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/DataTable.h>

namespace Veng
{
    /// @brief AssetTypes::DataTable loader: decodes the cooked row block and streams its schema.
    ///
    /// The schema is an ordinary cooked dependency, resolved through the manager like a material
    /// instance's parent; the row block itself is self-describing enough for a key lookup, so the
    /// table's stride is only cross-checked against the schema once the dependency is resident.
    /// The table never loads the assets its AssetRef cells name.
    class DataTableLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::DataTable.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::DataTable; }

        /// @brief Decodes the cooked table blob into a LoadJob producing a resident DataTable.
        [[nodiscard]] AssetResult<Detail::LoadJob> Load(AssetManager& manager,
                                                        Renderer::Context& context,
                                                        TaskSystem& tasks, TypeRegistry& types,
                                                        AssetId id, std::span<const u8> cooked,
                                                        bool async) const override;
    };
}
