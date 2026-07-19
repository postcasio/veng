#include <Veng/Asset/DataTable.h>

#include <Veng/Reflection/Serialize.h>

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

namespace Veng
{
    namespace
    {
        // The runtime and cooked key-kind vocabularies are one enum with two spellings; the
        // loader converts by cast, so a divergence must be a compile error, not a misread index.
        static_assert(static_cast<u32>(TableKeyKind::Integer) ==
                      static_cast<u32>(CookedTableKeyKind::Integer));
        static_assert(static_cast<u32>(TableKeyKind::String) ==
                      static_cast<u32>(CookedTableKeyKind::String));

        // A column is a reflected field whose owning record is the row, so it addresses its cell
        // at offset zero and the shared walker encodes and decodes it unchanged.
        FieldDescriptor CellDescriptor(const TableColumnDescriptor& column)
        {
            FieldDescriptor field;
            field.Name = column.Name;
            field.Type = column.Type;
            field.Class = column.Class;
            field.Offset = 0;
            return field;
        }
    }

    Result<TableSchemaLayout> LayOutTableSchema(const std::span<TableColumnDescriptor> columns,
                                                const std::string_view keyName,
                                                const TypeRegistry& types)
    {
        if (columns.empty())
        {
            return std::unexpected(string{"a schema declares no columns"});
        }

        TableSchemaLayout layout;
        u32 cursor = 0;
        bool arithmetic = true;
        bool allFixed = true;

        for (usize i = 0; i < columns.size(); ++i)
        {
            TableColumnDescriptor& column = columns[i];

            if (column.Name.empty())
            {
                return std::unexpected(fmt::format("column {} has an empty name", i));
            }
            // A cooked column name is a fixed-capacity char array, so an over-long name would be
            // silently truncated into a name no row could address.
            if (column.Name.size() >= ShaderNameCapacity)
            {
                return std::unexpected(fmt::format("column '{}' name exceeds {} bytes", column.Name,
                                                   ShaderNameCapacity - 1));
            }
            for (usize j = 0; j < i; ++j)
            {
                if (columns[j].Name == column.Name)
                {
                    return std::unexpected(
                        fmt::format("column '{}' is declared more than once", column.Name));
                }
            }

            if (!types.IsRegistered(column.Type))
            {
                return std::unexpected(
                    fmt::format("column '{}' has no registered reflected type", column.Name));
            }
            const TypeInfo& info = types.Info(column.Type);
            if (info.Class == FieldClass::Reference)
            {
                return std::unexpected(
                    fmt::format("column '{}': type '{}' is an intra-scene entity reference, which "
                                "no table row can resolve",
                                column.Name, info.QualifiedName));
            }

            column.Class = info.Class;

            const bool fixed = TableCellIsFixedSize(info.Class);
            allFixed = allFixed && fixed;

            if (arithmetic && fixed)
            {
                column.Offset = cursor;
                cursor += TableCellEncodedSize(info.Class, info);
            }
            else
            {
                column.Offset = CookedTableColumnOffsetUnresolved;
                arithmetic = false;
            }
        }

        layout.FixedStride = allFixed;
        layout.RowStride = allFixed ? cursor : 0;

        const auto key = std::ranges::find(columns, keyName, &TableColumnDescriptor::Name);
        if (key == columns.end())
        {
            return std::unexpected(fmt::format("key column '{}' is not declared", keyName));
        }

        const optional<TableKeyKind> keyKind = TableKeyKindForType(key->Type, key->Class);
        if (!keyKind)
        {
            return std::unexpected(fmt::format(
                "key column '{}' has type '{}'; a key column must be an integer or a "
                "string, the only types with a total order and a stable cooked encoding",
                keyName, types.Info(key->Type).QualifiedName));
        }
        layout.KeyKind = *keyKind;
        layout.KeyColumn = static_cast<u32>(std::distance(columns.begin(), key));

        return layout;
    }

    Ref<TableSchema> TableSchema::Create(vector<TableColumnDescriptor> columns, const u32 keyColumn,
                                         const TableKeyKind keyKind, const bool fixedStride,
                                         const u32 rowStride)
    {
        return Ref<TableSchema>(
            new TableSchema(std::move(columns), keyColumn, keyKind, fixedStride, rowStride));
    }

    TableSchema::TableSchema(vector<TableColumnDescriptor> columns, const u32 keyColumn,
                             const TableKeyKind keyKind, const bool fixedStride,
                             const u32 rowStride)
        : m_Columns(std::move(columns)), m_KeyColumn(keyColumn), m_KeyKind(keyKind),
          m_FixedStride(fixedStride), m_RowStride(rowStride)
    {
        VE_ASSERT(m_KeyColumn < m_Columns.size(), "TableSchema: key column {} is out of range",
                  m_KeyColumn);
    }

    const TableColumnDescriptor* TableSchema::FindColumn(const std::string_view name) const
    {
        const auto it = std::ranges::find(m_Columns, name, &TableColumnDescriptor::Name);
        return it == m_Columns.end() ? nullptr : &*it;
    }

    optional<u32> TableSchema::FindColumnIndex(const std::string_view name) const
    {
        const auto it = std::ranges::find(m_Columns, name, &TableColumnDescriptor::Name);
        if (it == m_Columns.end())
        {
            return std::nullopt;
        }
        return static_cast<u32>(std::distance(m_Columns.begin(), it));
    }

    Ref<DataTable> DataTable::Create(Contents contents, const TypeRegistry& types)
    {
        return Ref<DataTable>(new DataTable(std::move(contents), types));
    }

    DataTable::DataTable(Contents contents, const TypeRegistry& types)
        : m_Contents(std::move(contents)), m_Types(&types),
          m_RowCount(static_cast<u32>(m_Contents.Keys.size()))
    {
    }

    const TableSchema& DataTable::GetSchema() const
    {
        // AssetHandle exposes only operator->, which already asserts residency.
        return *m_Contents.Schema.operator->();
    }

    std::span<const u8> DataTable::GetRowBytes(const u32 row) const
    {
        VE_ASSERT(row < m_RowCount, "DataTable: row {} is out of range ({} rows)", row, m_RowCount);

        const std::span<const u8> rows{m_Contents.Rows};
        if (m_Contents.FixedStride)
        {
            return rows.subspan(static_cast<usize>(row) * m_Contents.RowStride,
                                m_Contents.RowStride);
        }

        const usize begin = m_Contents.RowOffsets[row];
        const usize end =
            row + 1 < m_RowCount ? m_Contents.RowOffsets[row + 1] : m_Contents.Rows.size();
        return rows.subspan(begin, end - begin);
    }

    const TableColumnDescriptor& DataTable::RequireColumn(const std::string_view name,
                                                          const TypeId expected) const
    {
        const TableColumnDescriptor* const column = GetSchema().FindColumn(name);
        VE_ASSERT(column != nullptr, "DataTable: no column named '{}'", name);
        VE_ASSERT(column->Type == expected,
                  "DataTable: column '{}' is type {:#018x}, read as type {:#018x}", name,
                  column->Type, expected);
        return *column;
    }

    TableColumn<AssetId> DataTable::GetAssetIdColumn(const std::string_view name) const
    {
        const TableColumnDescriptor* const column = GetSchema().FindColumn(name);
        VE_ASSERT(column != nullptr, "DataTable: no column named '{}'", name);
        VE_ASSERT(column->Class == FieldClass::AssetHandle,
                  "DataTable: column '{}' is not an asset-handle column", name);
        VE_ASSERT(column->Offset != CookedTableColumnOffsetUnresolved,
                  "DataTable: column '{}' has no constant cell offset", name);
        return TableColumn<AssetId>(*this, column->Offset);
    }

    Result<u32> DataTable::CellOffset(const u32 row, const u32 columnIndex) const
    {
        const std::span<const TableColumnDescriptor> columns = GetSchema().GetColumns();
        VE_ASSERT(columnIndex < columns.size(), "DataTable: column {} is out of range",
                  columnIndex);

        if (columns[columnIndex].Offset != CookedTableColumnOffsetUnresolved)
        {
            return columns[columnIndex].Offset;
        }

        // Past the first variable-size column the offset is not arithmetic: decode each
        // preceding cell purely to advance the cursor by whatever it consumed.
        const std::span<const u8> bytes = GetRowBytes(row);
        usize cursor = 0;
        for (u32 i = 0; i < columnIndex; ++i)
        {
            const FieldDescriptor field = CellDescriptor(columns[i]);
            const ReflectedStorage scratch(m_Types->Info(columns[i].Type));
            if (VoidResult read = ReadFieldValue(bytes, cursor, scratch.Get(), field, *m_Types);
                !read)
            {
                return std::unexpected(fmt::format("data table: row {}: column '{}': {}", row,
                                                   columns[i].Name, read.error()));
            }
        }
        return static_cast<u32>(cursor);
    }

    VoidResult DataTable::ReadCellInto(const u32 row, const TableColumnDescriptor& column,
                                       void* out) const
    {
        const optional<u32> columnIndex = GetSchema().FindColumnIndex(column.Name);
        VE_ASSERT(columnIndex.has_value(), "DataTable: column '{}' is not in this schema",
                  column.Name);

        const Result<u32> offset = CellOffset(row, *columnIndex);
        if (!offset)
        {
            return std::unexpected(offset.error());
        }

        const std::span<const u8> bytes = GetRowBytes(row);
        usize cursor = *offset;
        const FieldDescriptor field = CellDescriptor(column);
        if (VoidResult read = ReadFieldValue(bytes, cursor, out, field, *m_Types); !read)
        {
            return std::unexpected(
                fmt::format("data table: row {}: column '{}': {}", row, column.Name, read.error()));
        }
        return {};
    }

    VoidResult DataTable::ReadRowInto(const u32 row, void* out, const TypeInfo& type) const
    {
        const std::span<const TableColumnDescriptor> columns = GetSchema().GetColumns();
        const std::span<const u8> bytes = GetRowBytes(row);

        usize cursor = 0;
        for (const TableColumnDescriptor& column : columns)
        {
            const auto field = std::ranges::find(type.Fields, column.Name, &FieldDescriptor::Name);
            const bool bound = field != type.Fields.end() && field->Type == column.Type;

            // A column the row struct does not declare is still decoded — the cursor must clear
            // its bytes — but into scratch storage that is thrown away.
            void* destination = nullptr;
            optional<ReflectedStorage> scratch;
            if (bound)
            {
                destination = static_cast<u8*>(out) + field->Offset;
            }
            else
            {
                scratch.emplace(m_Types->Info(column.Type));
                destination = scratch->Get();
            }

            const FieldDescriptor descriptor = bound ? *field : CellDescriptor(column);
            if (VoidResult read = ReadFieldValue(bytes, cursor, destination, descriptor, *m_Types);
                !read)
            {
                return std::unexpected(fmt::format("data table: row {}: column '{}': {}", row,
                                                   column.Name, read.error()));
            }
        }
        return {};
    }

    Result<std::string_view> DataTable::GetStringCell(const u32 row,
                                                      const std::string_view name) const
    {
        const optional<u32> columnIndex = GetSchema().FindColumnIndex(name);
        VE_ASSERT(columnIndex.has_value(), "DataTable: no column named '{}'", name);

        const TableColumnDescriptor& column = GetSchema().GetColumns()[*columnIndex];
        VE_ASSERT(column.Class == FieldClass::String, "DataTable: column '{}' is not a string",
                  name);

        const Result<u32> offset = CellOffset(row, *columnIndex);
        if (!offset)
        {
            return std::unexpected(offset.error());
        }

        // A string cell is a u32 length followed by its bytes, in the row itself — so the text is
        // a view into the resident row block rather than a copy.
        const std::span<const u8> bytes = GetRowBytes(row);
        if (static_cast<usize>(*offset) + sizeof(u32) > bytes.size())
        {
            return std::unexpected(
                fmt::format("data table: row {}: column '{}': truncated string length", row, name));
        }
        u32 length = 0;
        std::memcpy(&length, bytes.data() + *offset, sizeof(length));
        const usize begin = static_cast<usize>(*offset) + sizeof(u32);
        if (begin + length > bytes.size())
        {
            return std::unexpected(
                fmt::format("data table: row {}: column '{}': truncated string", row, name));
        }
        return std::string_view(reinterpret_cast<const char*>(bytes.data()) + begin, length);
    }

    optional<u32> DataTable::FindRow(const i64 key) const
    {
        VE_ASSERT(m_Contents.KeyKind == TableKeyKind::Integer,
                  "DataTable: integer key lookup on a string-keyed table");

        const auto it = std::ranges::lower_bound(m_Contents.Keys, key, {}, &CookedTableKey::IntKey);
        if (it == m_Contents.Keys.end() || it->IntKey != key)
        {
            return std::nullopt;
        }
        return it->RowIndex;
    }

    optional<u32> DataTable::FindRow(const std::string_view key) const
    {
        VE_ASSERT(m_Contents.KeyKind == TableKeyKind::String,
                  "DataTable: string key lookup on an integer-keyed table");

        const auto heapString = [this](const CookedTableStringSpan span) -> std::string_view
        {
            if (span.Length == 0)
            {
                return {};
            }
            return std::string_view(reinterpret_cast<const char*>(m_Contents.KeyHeap.data()) +
                                        span.Offset,
                                    span.Length);
        };

        const auto it = std::ranges::lower_bound(m_Contents.Keys, key, {},
                                                 [&heapString](const CookedTableKey& entry)
                                                 { return heapString(entry.StringKey); });
        if (it == m_Contents.Keys.end() || heapString(it->StringKey) != key)
        {
            return std::nullopt;
        }
        return it->RowIndex;
    }
}
