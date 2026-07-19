#pragma once

#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/DataTable.h>
#include <Veng/Cook/Types.h>

namespace Veng::Cook
{
    /// @brief One column parsed from a `*.tableschema.json` source, with its resolved row offset.
    struct TableSchemaSourceColumn
    {
        /// @brief The authored column name; unique within the schema.
        string Name;
        /// @brief The authored cell type.
        TableColumnKind Kind = TableColumnKind::Int;
        /// @brief Byte offset of this column's cell within a row record, assigned by the layout pass.
        u32 Offset = 0;
        /// @brief For an AssetRef column, the asset type its cells must reference; invalid otherwise.
        AssetTypeId ReferencedType;
    };

    /// @brief A parsed `*.tableschema.json`: its columns, key column, and computed row layout.
    ///
    /// Shared by the schema importer (which serializes it) and the table importer (which
    /// re-parses the schema source it resolved, to validate rows against the same layout), so the
    /// two cannot disagree about a cell's offset.
    struct TableSchemaSource
    {
        /// @brief The columns in authored declaration order.
        vector<TableSchemaSourceColumn> Columns;
        /// @brief Index of the key column within Columns.
        u32 KeyColumn = 0;
        /// @brief Byte size of one row record laid out against Columns.
        u32 RowStride = 0;
    };

    /// @brief Parses a `*.tableschema.json` source and assigns each column its row offset.
    ///
    /// Columns are laid out in declaration order, each aligned to its cell alignment, the stride
    /// rounded up to eight. Validates that names are unique, kinds are known, an `AssetRef`
    /// column's `assetType` names a registered type, and the key column exists and is `Int` or
    /// `String`. Errors are located: `"table schema: '<file>': <reason>"`.
    /// @param file   Path to the `*.tableschema.json` source.
    /// @param types  Registry an `AssetRef` column's `assetType` name resolves through.
    /// @return The parsed schema, or a located error.
    [[nodiscard]] Result<TableSchemaSource> ParseTableSchema(const path& file,
                                                             const AssetTypeRegistry& types);
}
