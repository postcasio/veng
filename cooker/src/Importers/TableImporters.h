#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a `*.tableschema.json` source into a CookedTableSchemaHeader plus its columns.
    ///
    /// The layout pass runs here: each column is assigned a row offset and the schema's row stride
    /// is fixed, so every table cooked against the schema shares one authority for where a cell sits.
    class TableSchemaImporter final : public AssetImporter
    {
    public:
        /// @brief Returns AssetTypes::TableSchema.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::TableSchema; }

        /// @brief Cooks the table schema described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };

    /// @brief Cooks a `*.table.json` source into fixed-stride rows, a string heap, and a key index.
    ///
    /// Resolves the table's schema through CookContext::Resolve — which is what records the
    /// schema as a cook dependency — re-parses it, and validates every row against it: an unknown
    /// or missing column, a kind mismatch, a duplicate key, an unresolvable AssetRef, and an
    /// AssetRef whose target is the wrong asset type are all located cook errors. Each AssetRef
    /// cell resolves through CookContext::Resolve too, putting every referenced asset into the
    /// cooked dependency graph without the runtime ever loading it.
    class DataTableImporter final : public AssetImporter
    {
    public:
        /// @brief Returns AssetTypes::DataTable.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::DataTable; }

        /// @brief Cooks the data table described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };
}
