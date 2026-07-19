#pragma once

#include <cstring>
#include <span>
#include <string_view>

#include <Veng/Veng.h>
#include <Veng/Assert.h>
#include <Veng/Result.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Reflection/ReflectionTypes.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>

namespace Veng
{
    /// @brief How a table's key index is ordered and binary-searched.
    ///
    /// The engine spelling of CookedTableKeyKind; the enumerator values match one for one, so the
    /// loader's conversion is a cast guarded by a static_assert. It describes the *index*, not a
    /// cell type — a cell's type is a reflection TypeId like any other reflected value.
    enum class TableKeyKind : u32
    {
        /// @brief Keys are integers, widened to i64 and ordered numerically.
        Integer = 0,
        /// @brief Keys are UTF-8 strings, ordered byte-wise lexicographically.
        String = 1,
    };

    /// @brief True when a field class encodes to a byte count that does not depend on the value.
    ///
    /// The record encoding writes these classes as a flat copy of the type's bytes (an
    /// AssetHandle as its leading u64, a Reference as two u32s), so a column of such a type sits
    /// at a computable offset and supports the zero-copy TableColumn path. String, Struct,
    /// Variant, and Array are length-prefixed or recursive and so vary per value.
    /// @param cls  The field class to test.
    /// @return True when the class encodes to a constant byte count.
    [[nodiscard]] constexpr bool TableCellIsFixedSize(const FieldClass cls)
    {
        switch (cls)
        {
        case FieldClass::Scalar:
        case FieldClass::Vector:
        case FieldClass::Quaternion:
        case FieldClass::Matrix:
        case FieldClass::Enum:
        case FieldClass::AssetHandle:
        case FieldClass::Reference:
            return true;
        case FieldClass::String:
        case FieldClass::Struct:
        case FieldClass::Variant:
        case FieldClass::Array:
            return false;
        }
        return false;
    }

    /// @brief Encoded byte count of a fixed-size cell of the given type.
    /// @param cls   The type's field class; must satisfy TableCellIsFixedSize.
    /// @param info  The column type's TypeInfo, supplying the leaf width.
    /// @return The number of bytes the record encoding writes for one such cell.
    [[nodiscard]] inline u32 TableCellEncodedSize(const FieldClass cls, const TypeInfo& info)
    {
        VE_ASSERT(TableCellIsFixedSize(cls), "TableCellEncodedSize: '{}' is not fixed-size",
                  info.QualifiedName);
        if (cls == FieldClass::AssetHandle)
        {
            // Only the handle's leading AssetId is recorded, never its cache indirection.
            return static_cast<u32>(sizeof(u64));
        }
        if (cls == FieldClass::Reference)
        {
            return static_cast<u32>(2 * sizeof(u32));
        }
        return static_cast<u32>(info.Size);
    }

    /// @brief The key-index ordering a reflected type may serve as a key column, if any.
    ///
    /// A key column needs a total order and a stable cooked encoding, which restricts it to the
    /// signed-representable integer scalars and string. Floats are excluded — an equality-keyed
    /// lookup over them is a trap — as is bool, which cannot key more than two rows. u64 is
    /// excluded because the key index widens every integer key to i64, and a u64 above 2^63 would
    /// sort as negative and break the binary search.
    /// @param type  The column's reflected type.
    /// @param cls   The column type's field class.
    /// @return The ordering the key index would use, or nullopt when the type cannot be a key.
    [[nodiscard]] inline optional<TableKeyKind> TableKeyKindForType(const TypeId type,
                                                                    const FieldClass cls)
    {
        if (cls == FieldClass::String)
        {
            return TableKeyKind::String;
        }
        if (cls == FieldClass::Scalar && (type == TypeIdOf<u8>() || type == TypeIdOf<i32>() ||
                                          type == TypeIdOf<u32>() || type == TypeIdOf<i64>()))
        {
            return TableKeyKind::Integer;
        }
        return std::nullopt;
    }

    /// @brief One column of a loaded TableSchema.
    ///
    /// Deliberately the serialization quadruple of a FieldDescriptor: a column *is* a reflected
    /// field, so the same walker encodes, decodes, and validates it. Class is denormalised from
    /// the column type's TypeInfo exactly as FieldDescriptor denormalises a field's.
    struct TableColumnDescriptor
    {
        /// @brief The authored column name; unique within the schema, and the key rows address by.
        string Name;
        /// @brief The column's reflected type; the single authority on its encoding and editing.
        TypeId Type = InvalidTypeId;
        /// @brief The column type's meta-kind, denormalised so a walk avoids a registry lookup.
        FieldClass Class = FieldClass::Scalar;
        /// @brief Byte offset of the cell within a row, or CookedTableColumnOffsetUnresolved.
        ///
        /// Resolved only while every preceding column is fixed-size; a column past the first
        /// variable-size one is reached by walking the row's preceding cells instead.
        u32 Offset = CookedTableColumnOffsetUnresolved;
    };

    /// @brief A declared set of reflected columns plus the key column rows are addressed by.
    ///
    /// Cooked from a `*.tableschema.json` source and loaded by AssetId like any asset. A DataTable
    /// holds a handle to the schema it was cooked against; the schema owns the column names,
    /// reflected types, and cell offsets the table's accessors resolve through. Carries no GPU
    /// resource.
    class TableSchema
    {
    public:
        /// @brief Creates a schema from its decoded columns.
        /// @param columns      The columns in authored declaration order.
        /// @param keyColumn    Index of the key column within columns.
        /// @param keyKind      The ordering the key index is built under.
        /// @param fixedStride  Whether every column's type encodes to a constant byte count.
        /// @param rowStride    Byte size of one row; meaningful only when fixedStride is true.
        /// @return The constructed schema.
        static Ref<TableSchema> Create(vector<TableColumnDescriptor> columns, u32 keyColumn,
                                       TableKeyKind keyKind, bool fixedStride, u32 rowStride);

        /// @brief Returns the columns in authored declaration order.
        [[nodiscard]] std::span<const TableColumnDescriptor> GetColumns() const
        {
            return m_Columns;
        }

        /// @brief Returns the index of the key column within GetColumns().
        [[nodiscard]] u32 GetKeyColumnIndex() const { return m_KeyColumn; }

        /// @brief Returns the key column's descriptor.
        [[nodiscard]] const TableColumnDescriptor& GetKeyColumn() const
        {
            return m_Columns[m_KeyColumn];
        }

        /// @brief Returns the ordering the key index is built under.
        [[nodiscard]] TableKeyKind GetKeyKind() const { return m_KeyKind; }

        /// @brief Returns whether every column's type encodes to a constant byte count.
        ///
        /// A cooked-blob property, not a format contract: the accessor API is identical either
        /// way, so no consumer needs to branch on it.
        [[nodiscard]] bool IsFixedStride() const { return m_FixedStride; }

        /// @brief Returns the byte size of one row; meaningful only when IsFixedStride().
        [[nodiscard]] u32 GetRowStride() const { return m_RowStride; }

        /// @brief Looks a column up by name.
        /// @param name  The authored column name.
        /// @return The column's descriptor, or nullptr when the schema declares no such column.
        [[nodiscard]] const TableColumnDescriptor* FindColumn(std::string_view name) const;

        /// @brief Returns the index of a column by name.
        /// @param name  The authored column name.
        /// @return The column's index within GetColumns(), or nullopt when there is no such column.
        [[nodiscard]] optional<u32> FindColumnIndex(std::string_view name) const;

    private:
        TableSchema(vector<TableColumnDescriptor> columns, u32 keyColumn, TableKeyKind keyKind,
                    bool fixedStride, u32 rowStride);

        /// @brief The columns in authored declaration order.
        vector<TableColumnDescriptor> m_Columns;
        /// @brief Index of the key column within m_Columns.
        u32 m_KeyColumn = 0;
        /// @brief The ordering the key index is built under.
        TableKeyKind m_KeyKind = TableKeyKind::Integer;
        /// @brief Whether every column's type encodes to a constant byte count.
        bool m_FixedStride = false;
        /// @brief Byte size of one row; meaningful only when m_FixedStride.
        u32 m_RowStride = 0;
    };

    /// @brief AssetTypeTrait specialization mapping TableSchema to AssetTypes::TableSchema.
    template <>
    struct AssetTypeTrait<TableSchema>
    {
        /// @brief The asset type tag for TableSchema.
        static constexpr AssetTypeId Type = AssetTypes::TableSchema;
    };

    class DataTable;

    /// @brief A typed zero-copy view of one fixed-size column, resolved once and indexed per row.
    ///
    /// Holds the table and the column's cell offset, so `column[row]` is a bounds-checked flat
    /// read with no per-access name lookup and no decode — the path that keeps a large table fast.
    /// A view is valid only while the table it was resolved from is alive.
    /// @tparam T  The element type; must be the column's reflected type, or AssetId for a handle column.
    template <class T>
    class TableColumn
    {
    public:
        /// @brief Constructs a view over a table's column at a known cell offset.
        /// @param table   The table the view reads; must outlive the view.
        /// @param offset  Byte offset of the column's cell within a row.
        TableColumn(const DataTable& table, const u32 offset) : m_Table(&table), m_Offset(offset) {}

        /// @brief Reads the column's cell in the given row.
        /// @param row  Row index, below the table's row count.
        /// @return The decoded cell value.
        [[nodiscard]] T operator[](u32 row) const;

    private:
        /// @brief The table this view reads.
        const DataTable* m_Table;
        /// @brief Byte offset of the column's cell within a row.
        u32 m_Offset;
    };

    /// @brief Rows of reflected data, keyed and validated against a TableSchema.
    ///
    /// Cooked from a `*.table.json` source. A row is a sequence of cells in column order, each
    /// encoded by the engine's one record encoding (Veng/Reflection/Serialize.h) — so a column
    /// may be any reflected type, nested structs and arrays included, and the table introduces no
    /// second serializer. Rows are addressed through a row directory, or arithmetically when
    /// every column is fixed-size; the accessors read identically under both. A sorted, unique
    /// key index — a separate structure from the directory — makes FindRow an allocation-free
    /// binary search. The whole table is resident and it carries no GPU resource.
    ///
    /// An asset-handle cell yields a bare AssetId: the table never loads what it references, so
    /// the consumer decides what to load and when. The cook records each referenced asset as a
    /// dependency, which is what keeps the recook graph correct.
    class DataTable
    {
    public:
        /// @brief The decoded parts of a cooked table blob, as the loader hands them over.
        struct Contents
        {
            /// @brief Handle to the schema the rows were cooked against.
            AssetHandle<TableSchema> Schema;
            /// @brief The ordering the key index is built under.
            TableKeyKind KeyKind = TableKeyKind::Integer;
            /// @brief Whether the rows are a fixed stride and the directory is omitted.
            bool FixedStride = false;
            /// @brief Byte size of one row; meaningful only when FixedStride.
            u32 RowStride = 0;
            /// @brief The row region: every row's cells, back to back.
            vector<u8> Rows;
            /// @brief Byte offset of each row within Rows; empty when FixedStride.
            vector<u32> RowOffsets;
            /// @brief The key index, sorted ascending and unique.
            vector<CookedTableKey> Keys;
            /// @brief The bytes String key spans address.
            vector<u8> KeyHeap;
        };

        /// @brief Creates a table from a decoded blob's contents.
        /// @param contents  The decoded blob parts; validated by the loader before this call.
        /// @param types     Registry resolving column types; borrowed and must outlive the table.
        /// @return The constructed table.
        static Ref<DataTable> Create(Contents contents, const TypeRegistry& types);

        /// @brief Returns the handle to the schema these rows were cooked against.
        [[nodiscard]] const AssetHandle<TableSchema>& GetSchemaHandle() const
        {
            return m_Contents.Schema;
        }

        /// @brief Returns the schema these rows were cooked against.
        /// @pre The schema handle is resident, which the loader's dependency record guarantees.
        [[nodiscard]] const TableSchema& GetSchema() const;

        /// @brief Returns the type registry the table decodes cells against.
        [[nodiscard]] const TypeRegistry& GetTypeRegistry() const { return *m_Types; }

        /// @brief Returns the number of rows.
        [[nodiscard]] u32 GetRowCount() const { return m_RowCount; }

        /// @brief Returns the ordering the key index is built under.
        [[nodiscard]] TableKeyKind GetKeyKind() const { return m_Contents.KeyKind; }

        /// @brief Finds the row carrying an integer key, by binary search over the key index.
        /// @param key  The key value to look up.
        /// @return The row index, or nullopt when no row carries the key.
        /// @pre The key kind is Integer; calling this on a string-keyed table is API misuse.
        [[nodiscard]] optional<u32> FindRow(i64 key) const;

        /// @brief Finds the row carrying a string key, by binary search over the key index.
        /// @param key  The key value to look up.
        /// @return The row index, or nullopt when no row carries the key.
        /// @pre The key kind is String; calling this on an integer-keyed table is API misuse.
        [[nodiscard]] optional<u32> FindRow(std::string_view key) const;

        /// @brief Returns the raw encoded bytes of one row.
        /// @param row  Row index, below GetRowCount().
        /// @return The row's cell bytes.
        [[nodiscard]] std::span<const u8> GetRowBytes(u32 row) const;

        /// @brief Resolves a fixed-size column by name into a typed zero-copy view.
        ///
        /// The type and offset checks are fatal, not recoverable: naming a column that does not
        /// exist, reading it as the wrong type, or reading a column whose cell has no constant
        /// offset is API misuse in consumer code. A malformed blob is the recoverable case and is
        /// rejected by the loader as AssetError::Corrupt instead.
        /// @tparam T    The element type; must be the column's reflected type.
        /// @param name  The authored column name.
        /// @return A view over the column.
        template <class T>
        [[nodiscard]] TableColumn<T> GetColumn(const std::string_view name) const
        {
            const TableColumnDescriptor& column = RequireColumn(name, TypeIdOf<T>());
            VE_ASSERT(TableCellIsFixedSize(column.Class),
                      "DataTable: column '{}' is variable-size; read it with ReadCell", name);
            VE_ASSERT(column.Offset != CookedTableColumnOffsetUnresolved,
                      "DataTable: column '{}' has no constant cell offset; read it with ReadCell",
                      name);
            VE_ASSERT(sizeof(T) == TableCellEncodedSize(column.Class, m_Types->Info(column.Type)),
                      "DataTable: column '{}' encodes to a different width than {} bytes", name,
                      sizeof(T));
            return TableColumn<T>(*this, column.Offset);
        }

        /// @brief Resolves an asset-handle column by name into a view yielding bare AssetIds.
        ///
        /// The separate entry point exists because a cell stores only the handle's leading
        /// AssetId — a table never loads what it references, so there is no handle to hand back.
        /// @param name  The authored column name.
        /// @return A view yielding the referenced AssetId per row.
        [[nodiscard]] TableColumn<AssetId> GetAssetIdColumn(std::string_view name) const;

        /// @brief Decodes one cell into a reflected value of the column's type.
        ///
        /// The general read path: it serves every column, including the variable-size ones the
        /// zero-copy TableColumn cannot address. Reaching a cell past the first variable-size
        /// column walks that row's preceding cells, so a hot inner loop should prefer GetColumn
        /// where the column allows it.
        /// @tparam T    The destination type; must be the column's reflected type.
        /// @param row   Row index, below GetRowCount().
        /// @param name  The authored column name.
        /// @param out   Destination value, default-constructed by the caller.
        /// @return Empty on success; an error string when the encoded row is malformed.
        template <class T>
        VoidResult ReadCell(const u32 row, const std::string_view name, T& out) const
        {
            const TableColumnDescriptor& column = RequireColumn(name, TypeIdOf<T>());
            return ReadCellInto(row, column, &out);
        }

        /// @brief Reads a whole row into a reflected struct, binding cells to fields by name.
        ///
        /// The typed row bridge: each column whose name and reflected type match a field of T is
        /// decoded straight into that field; a column T does not declare is decoded and
        /// discarded, and a field no column declares keeps its caller-constructed value — the
        /// same drift tolerance the record encoding gives a struct.
        /// @tparam T   The row struct; must be a registered Struct-class type.
        /// @param row  Row index, below GetRowCount().
        /// @param out  Destination value, default-constructed by the caller.
        /// @return Empty on success; an error string when the encoded row is malformed.
        template <class T>
        VoidResult ReadRow(const u32 row, T& out) const
        {
            return ReadRowInto(row, &out, m_Types->Info(TypeIdOf<T>()));
        }

        /// @brief Reads a string cell as a view into the row's encoded bytes, without copying.
        /// @param row   Row index, below GetRowCount().
        /// @param name  The authored column name; must name a String-class column.
        /// @return The cell's text, or an error string when the encoded row is malformed.
        [[nodiscard]] Result<std::string_view> GetStringCell(u32 row, std::string_view name) const;

    private:
        DataTable(Contents contents, const TypeRegistry& types);

        /// @brief Resolves a column by name, asserting it exists and carries the expected type.
        [[nodiscard]] const TableColumnDescriptor& RequireColumn(std::string_view name,
                                                                 TypeId expected) const;

        /// @brief Byte offset of a column's cell within a row, walking preceding cells if needed.
        [[nodiscard]] Result<u32> CellOffset(u32 row, u32 columnIndex) const;

        /// @brief Decodes one cell into caller-owned storage of the column's type.
        [[nodiscard]] VoidResult ReadCellInto(u32 row, const TableColumnDescriptor& column,
                                              void* out) const;

        /// @brief Decodes a whole row into a reflected struct instance.
        [[nodiscard]] VoidResult ReadRowInto(u32 row, void* out, const TypeInfo& type) const;

        /// @brief The decoded blob parts.
        Contents m_Contents;
        /// @brief Registry resolving column types; host-owned and outlives the table.
        const TypeRegistry* m_Types = nullptr;
        /// @brief Number of rows, cross-validated by the loader against every length witness.
        u32 m_RowCount = 0;
    };

    /// @brief AssetTypeTrait specialization mapping DataTable to AssetTypes::DataTable.
    template <>
    struct AssetTypeTrait<DataTable>
    {
        /// @brief The asset type tag for DataTable.
        static constexpr AssetTypeId Type = AssetTypes::DataTable;
    };

    template <class T>
    T TableColumn<T>::operator[](const u32 row) const
    {
        const std::span<const u8> bytes = m_Table->GetRowBytes(row);
        VE_ASSERT(static_cast<usize>(m_Offset) + sizeof(T) <= bytes.size(),
                  "DataTable: column cell at offset {} is outside the {}-byte row", m_Offset,
                  bytes.size());

        T value{};
        std::memcpy(&value, bytes.data() + m_Offset, sizeof(T));
        return value;
    }
}
