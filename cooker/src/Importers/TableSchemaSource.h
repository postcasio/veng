#pragma once

#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/DataTable.h>
#include <Veng/Cook/Types.h>
#include <Veng/Reflection/TypeRegistry.h>

namespace Veng::Cook
{
    /// @brief One column parsed from a `*.tableschema.json` source, with its resolved cell offset.
    struct TableSchemaSourceColumn
    {
        /// @brief The authored column name; unique within the schema.
        string Name;
        /// @brief The column's reflected type, resolved from its authored qualified name.
        TypeId Type = InvalidTypeId;
        /// @brief The column type's meta-kind, denormalised from its TypeInfo.
        FieldClass Class = FieldClass::Scalar;
        /// @brief Byte offset of the cell within a row, or CookedTableColumnOffsetUnresolved.
        u32 Offset = CookedTableColumnOffsetUnresolved;
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
        /// @brief The ordering the key index is built under, derived from the key column's type.
        TableKeyKind KeyKind = TableKeyKind::Integer;
        /// @brief Whether every column's type encodes to a constant byte count.
        bool FixedStride = false;
        /// @brief Byte size of one row; meaningful only when FixedStride.
        u32 RowStride = 0;
    };

    /// @brief Parses a `*.tableschema.json` source and lays its columns out.
    ///
    /// A column names its type by the registered fully-qualified reflected type name — the same
    /// spelling a variant alternative's `"type"` tag uses. Cells are laid out in declaration
    /// order; a column keeps a constant offset only while every preceding column is fixed-size.
    /// Validates that names are unique, every type resolves in the registry, and the key column
    /// exists and has a type with a total order. Errors are located:
    /// `"table schema: '<file>': <reason>"`.
    /// @param file   Path to the `*.tableschema.json` source.
    /// @param types  Registry each column's type name resolves through.
    /// @return The parsed schema, or a located error.
    [[nodiscard]] Result<TableSchemaSource> ParseTableSchema(const path& file,
                                                             const TypeRegistry& types);
}
