#pragma once

#include <cstring>
#include <span>
#include <string_view>

#include <Veng/Veng.h>
#include <Veng/Assert.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Reflection/ReflectionTypes.h>

namespace Veng
{
    /// @brief The cell type of one table column, as the runtime spells it.
    ///
    /// The same closed vocabulary the cook writes as CookedTableColumnKind, restated in engine
    /// vocabulary so consumer code never includes the cooked-blob layout to name a kind. The
    /// enumerator values match one for one, so the loader's conversion is a cast guarded by a
    /// static_assert.
    enum class TableColumnKind : u32
    {
        /// @brief A boolean cell.
        Bool = 0,
        /// @brief A 64-bit signed integer cell; the only integer width a column may declare.
        Int = 1,
        /// @brief A 32-bit float cell; the only float width a column may declare.
        Float = 2,
        /// @brief A vec2 cell.
        Vec2 = 3,
        /// @brief A vec3 cell.
        Vec3 = 4,
        /// @brief A vec4 cell.
        Vec4 = 5,
        /// @brief A UTF-8 string cell, resident in the table's string heap.
        String = 6,
        /// @brief An AssetId cell referencing another asset; the invalid id is the unset reference.
        AssetRef = 7,
    };

    /// @brief The FieldClass a column kind presents as, so the inspector's widgets carry over.
    ///
    /// The two vocabularies do not line up and the mapping is therefore explicit: a column's Int
    /// is always i64 and its Float always f32, while FieldClass distinguishes neither width nor
    /// signedness. Bool/Int/Float land on Scalar, Vec2/Vec3/Vec4 on Vector, String on String, and
    /// AssetRef on AssetHandle — which is a *presentation* claim only: a cell stores a bare
    /// AssetId, never a handle, because a table never loads what it references.
    /// @param kind  The column kind to map.
    /// @return The FieldClass the kind presents as.
    [[nodiscard]] constexpr FieldClass FieldClassForColumnKind(const TableColumnKind kind)
    {
        switch (kind)
        {
        case TableColumnKind::Bool:
        case TableColumnKind::Int:
        case TableColumnKind::Float:
            return FieldClass::Scalar;
        case TableColumnKind::Vec2:
        case TableColumnKind::Vec3:
        case TableColumnKind::Vec4:
            return FieldClass::Vector;
        case TableColumnKind::String:
            return FieldClass::String;
        case TableColumnKind::AssetRef:
            return FieldClass::AssetHandle;
        }
        return FieldClass::Scalar;
    }

    /// @brief Byte size of one cell of the given column kind within a row record.
    /// @param kind  The column kind to size.
    /// @return The cell's size in bytes.
    [[nodiscard]] constexpr u32 TableCellSize(const TableColumnKind kind)
    {
        switch (kind)
        {
        case TableColumnKind::Bool:
            return 4;
        case TableColumnKind::Int:
            return 8;
        case TableColumnKind::Float:
            return 4;
        case TableColumnKind::Vec2:
            return 8;
        case TableColumnKind::Vec3:
            return 12;
        case TableColumnKind::Vec4:
            return 16;
        case TableColumnKind::String:
            return 8;
        case TableColumnKind::AssetRef:
            return 8;
        }
        return 0;
    }

    /// @brief Alignment one cell of the given column kind is placed on within a row record.
    /// @param kind  The column kind to align.
    /// @return The cell's alignment in bytes.
    [[nodiscard]] constexpr u32 TableCellAlignment(const TableColumnKind kind)
    {
        switch (kind)
        {
        case TableColumnKind::Bool:
        case TableColumnKind::Float:
        case TableColumnKind::Vec2:
        case TableColumnKind::Vec3:
        case TableColumnKind::Vec4:
        case TableColumnKind::String:
            return 4;
        case TableColumnKind::Int:
        case TableColumnKind::AssetRef:
            return 8;
        }
        return 4;
    }

    /// @brief One column of a loaded TableSchema.
    struct TableColumnDescriptor
    {
        /// @brief The authored column name; unique within the schema and the key rows address by.
        string Name;
        /// @brief The column's cell type.
        TableColumnKind Kind = TableColumnKind::Int;
        /// @brief Byte offset of this column's cell within a row record.
        u32 Offset = 0;
        /// @brief For an AssetRef column, the asset type its cells must reference; invalid otherwise.
        AssetTypeId ReferencedType;
    };

    /// @brief A declared set of typed columns plus the key column rows are addressed by.
    ///
    /// Cooked from a `*.tableschema.json` source and loaded by AssetId like any asset. A DataTable
    /// holds a handle to the schema it was cooked against; the schema owns the column names, kinds,
    /// and row offsets the table's typed accessors resolve through. Carries no GPU resource.
    class TableSchema
    {
    public:
        /// @brief Creates a schema from its decoded columns.
        /// @param columns    The columns in authored declaration order.
        /// @param keyColumn  Index of the key column within columns.
        /// @param rowStride  Byte size of one row record laid out against these columns.
        /// @return The constructed schema.
        static Ref<TableSchema> Create(vector<TableColumnDescriptor> columns, u32 keyColumn,
                                       u32 rowStride);

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

        /// @brief Returns the byte size of one row record laid out against these columns.
        [[nodiscard]] u32 GetRowStride() const { return m_RowStride; }

        /// @brief Looks a column up by name.
        /// @param name  The authored column name.
        /// @return The column's descriptor, or nullptr when the schema declares no such column.
        [[nodiscard]] const TableColumnDescriptor* FindColumn(std::string_view name) const;

    private:
        TableSchema(vector<TableColumnDescriptor> columns, u32 keyColumn, u32 rowStride);

        /// @brief The columns in authored declaration order.
        vector<TableColumnDescriptor> m_Columns;
        /// @brief Index of the key column within m_Columns.
        u32 m_KeyColumn = 0;
        /// @brief Byte size of one row record.
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

    /// @brief The column kind a typed accessor's element type reads.
    ///
    /// Specialized for every type TableColumn<T> supports; an unsupported T fails to compile
    /// rather than silently reinterpreting a cell.
    /// @tparam T  The accessor's element type.
    template <class T>
    struct TableColumnKindTrait;

    /// @brief TableColumnKindTrait for a Bool column.
    template <>
    struct TableColumnKindTrait<bool>
    {
        /// @brief The column kind bool reads.
        static constexpr TableColumnKind Kind = TableColumnKind::Bool;
    };

    /// @brief TableColumnKindTrait for an Int column.
    template <>
    struct TableColumnKindTrait<i64>
    {
        /// @brief The column kind i64 reads.
        static constexpr TableColumnKind Kind = TableColumnKind::Int;
    };

    /// @brief TableColumnKindTrait for a Float column.
    template <>
    struct TableColumnKindTrait<f32>
    {
        /// @brief The column kind f32 reads.
        static constexpr TableColumnKind Kind = TableColumnKind::Float;
    };

    /// @brief TableColumnKindTrait for a Vec2 column.
    template <>
    struct TableColumnKindTrait<vec2>
    {
        /// @brief The column kind vec2 reads.
        static constexpr TableColumnKind Kind = TableColumnKind::Vec2;
    };

    /// @brief TableColumnKindTrait for a Vec3 column.
    template <>
    struct TableColumnKindTrait<vec3>
    {
        /// @brief The column kind vec3 reads.
        static constexpr TableColumnKind Kind = TableColumnKind::Vec3;
    };

    /// @brief TableColumnKindTrait for a Vec4 column.
    template <>
    struct TableColumnKindTrait<vec4>
    {
        /// @brief The column kind vec4 reads.
        static constexpr TableColumnKind Kind = TableColumnKind::Vec4;
    };

    /// @brief TableColumnKindTrait for a String column.
    template <>
    struct TableColumnKindTrait<std::string_view>
    {
        /// @brief The column kind std::string_view reads.
        static constexpr TableColumnKind Kind = TableColumnKind::String;
    };

    /// @brief TableColumnKindTrait for an AssetRef column.
    template <>
    struct TableColumnKindTrait<AssetId>
    {
        /// @brief The column kind AssetId reads.
        static constexpr TableColumnKind Kind = TableColumnKind::AssetRef;
    };

    /// @brief A typed view of one column, resolved once and indexed per row.
    ///
    /// Holds the table and the column's row offset, so `column[row]` is a bounds-checked read of a
    /// fixed-stride cell with no per-access name lookup. A view is valid only while the table it
    /// was resolved from is alive.
    /// @tparam T  The element type, which fixes the column kind through TableColumnKindTrait.
    template <class T>
    class TableColumn
    {
    public:
        /// @brief Constructs a view over a table's column at a known row offset.
        /// @param table   The table the view reads; must outlive the view.
        /// @param offset  Byte offset of the column's cell within a row record.
        TableColumn(const DataTable& table, const u32 offset) : m_Table(&table), m_Offset(offset) {}

        /// @brief Reads the column's cell in the given row.
        /// @param row  Row index, below the table's row count.
        /// @return The decoded cell value.
        [[nodiscard]] T operator[](u32 row) const;

    private:
        /// @brief The table this view reads.
        const DataTable* m_Table;
        /// @brief Byte offset of the column's cell within a row record.
        u32 m_Offset;
    };

    /// @brief Rows of structured data, keyed and validated against a TableSchema.
    ///
    /// Cooked from a `*.table.json` source: fixed-stride row records over a string heap, plus a
    /// sorted key index so FindRow is an allocation-free binary search. The whole table is
    /// resident — tables are sized for full residency — and carries no GPU resource.
    ///
    /// An AssetRef cell yields a bare AssetId: the table never loads what it references, so the
    /// consumer decides what to load and when. The cook records each referenced asset as a
    /// dependency, which is what keeps the recook graph correct.
    class DataTable
    {
    public:
        /// @brief Creates a table from its decoded schema handle, rows, key index, and string heap.
        /// @param schema      Handle to the schema the rows were cooked against.
        /// @param keyKind     The key column's kind; Int or String.
        /// @param rowStride   Byte size of one row record.
        /// @param rows        The row records, row-major and tightly packed at rowStride.
        /// @param keys        The key index, sorted ascending and unique.
        /// @param stringHeap  The bytes String cells span into.
        /// @return The constructed table.
        static Ref<DataTable> Create(AssetHandle<TableSchema> schema, TableColumnKind keyKind,
                                     u32 rowStride, vector<u8> rows, vector<CookedTableKey> keys,
                                     vector<u8> stringHeap);

        /// @brief Returns the handle to the schema these rows were cooked against.
        [[nodiscard]] const AssetHandle<TableSchema>& GetSchemaHandle() const { return m_Schema; }

        /// @brief Returns the schema these rows were cooked against.
        /// @pre The schema handle is resident, which the loader's dependency record guarantees.
        [[nodiscard]] const TableSchema& GetSchema() const;

        /// @brief Returns the number of rows.
        [[nodiscard]] u32 GetRowCount() const { return static_cast<u32>(m_Keys.size()); }

        /// @brief Returns the key column's kind; Int or String.
        [[nodiscard]] TableColumnKind GetKeyKind() const { return m_KeyKind; }

        /// @brief Returns the byte size of one row record.
        [[nodiscard]] u32 GetRowStride() const { return m_RowStride; }

        /// @brief Finds the row carrying an integer key, by binary search over the key index.
        /// @param key  The key value to look up.
        /// @return The row index, or nullopt when no row carries the key.
        /// @pre The key column's kind is Int; calling this on a String-keyed table is API misuse.
        [[nodiscard]] optional<u32> FindRow(i64 key) const;

        /// @brief Finds the row carrying a string key, by binary search over the key index.
        /// @param key  The key value to look up.
        /// @return The row index, or nullopt when no row carries the key.
        /// @pre The key column's kind is String; calling this on an Int-keyed table is API misuse.
        [[nodiscard]] optional<u32> FindRow(std::string_view key) const;

        /// @brief Resolves a column by name into a typed view indexed per row.
        ///
        /// The kind check is fatal, not recoverable: naming a column that does not exist, or
        /// reading it as the wrong element type, is API misuse in consumer code. A malformed blob
        /// is the recoverable case and is rejected by the loader as AssetError::Corrupt instead.
        /// @tparam T    The element type; fixes the column kind through TableColumnKindTrait.
        /// @param name  The authored column name.
        /// @return A view over the column.
        template <class T>
        [[nodiscard]] TableColumn<T> GetColumn(const std::string_view name) const
        {
            const TableColumnDescriptor* const column = GetSchema().FindColumn(name);
            VE_ASSERT(column != nullptr, "DataTable: no column named '{}'", name);
            VE_ASSERT(column->Kind == TableColumnKindTrait<T>::Kind,
                      "DataTable: column '{}' is kind {}, read as kind {}", name,
                      static_cast<u32>(column->Kind),
                      static_cast<u32>(TableColumnKindTrait<T>::Kind));
            return TableColumn<T>(*this, column->Offset);
        }

        /// @brief Returns the raw bytes of one row record.
        /// @param row  Row index, below GetRowCount().
        /// @return The row's RowStride bytes.
        [[nodiscard]] std::span<const u8> GetRowBytes(u32 row) const;

        /// @brief Resolves a string cell's heap span into a view of the heap bytes.
        /// @param span  The cell's heap span.
        /// @return The string, or an empty view when the span is out of range.
        [[nodiscard]] std::string_view GetString(CookedTableStringSpan span) const;

    private:
        DataTable(AssetHandle<TableSchema> schema, TableColumnKind keyKind, u32 rowStride,
                  vector<u8> rows, vector<CookedTableKey> keys, vector<u8> stringHeap);

        /// @brief The schema the rows were cooked against; an ordinary streamed dependency.
        AssetHandle<TableSchema> m_Schema;
        /// @brief The key column's kind; Int or String.
        TableColumnKind m_KeyKind = TableColumnKind::Int;
        /// @brief Byte size of one row record.
        u32 m_RowStride = 0;
        /// @brief The row records, row-major and tightly packed at m_RowStride.
        vector<u8> m_Rows;
        /// @brief The key index, sorted ascending and unique.
        vector<CookedTableKey> m_Keys;
        /// @brief The bytes String cells span into.
        vector<u8> m_StringHeap;
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
        VE_ASSERT(m_Offset + TableCellSize(TableColumnKindTrait<T>::Kind) <= bytes.size(),
                  "DataTable: column cell at offset {} is outside the {}-byte row", m_Offset,
                  bytes.size());

        if constexpr (std::is_same_v<T, bool>)
        {
            u32 raw = 0;
            std::memcpy(&raw, bytes.data() + m_Offset, sizeof(raw));
            return raw != 0;
        }
        else if constexpr (std::is_same_v<T, std::string_view>)
        {
            CookedTableStringSpan span;
            std::memcpy(&span, bytes.data() + m_Offset, sizeof(span));
            return m_Table->GetString(span);
        }
        else if constexpr (std::is_same_v<T, AssetId>)
        {
            u64 raw = 0;
            std::memcpy(&raw, bytes.data() + m_Offset, sizeof(raw));
            return AssetId{raw};
        }
        else
        {
            T value{};
            std::memcpy(&value, bytes.data() + m_Offset, sizeof(T));
            return value;
        }
    }
}
