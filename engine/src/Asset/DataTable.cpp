#include <Veng/Asset/DataTable.h>

#include <algorithm>

namespace Veng
{
    namespace
    {
        // The runtime and cooked column-kind vocabularies are one enum with two spellings; the
        // loaders convert by cast, so a divergence must be a compile error rather than a misread cell.
        static_assert(static_cast<u32>(TableColumnKind::Bool) ==
                      static_cast<u32>(CookedTableColumnKind::Bool));
        static_assert(static_cast<u32>(TableColumnKind::Int) ==
                      static_cast<u32>(CookedTableColumnKind::Int));
        static_assert(static_cast<u32>(TableColumnKind::Float) ==
                      static_cast<u32>(CookedTableColumnKind::Float));
        static_assert(static_cast<u32>(TableColumnKind::Vec2) ==
                      static_cast<u32>(CookedTableColumnKind::Vec2));
        static_assert(static_cast<u32>(TableColumnKind::Vec3) ==
                      static_cast<u32>(CookedTableColumnKind::Vec3));
        static_assert(static_cast<u32>(TableColumnKind::Vec4) ==
                      static_cast<u32>(CookedTableColumnKind::Vec4));
        static_assert(static_cast<u32>(TableColumnKind::String) ==
                      static_cast<u32>(CookedTableColumnKind::String));
        static_assert(static_cast<u32>(TableColumnKind::AssetRef) ==
                      static_cast<u32>(CookedTableColumnKind::AssetRef));
    }

    Ref<TableSchema> TableSchema::Create(vector<TableColumnDescriptor> columns, const u32 keyColumn,
                                         const u32 rowStride)
    {
        return Ref<TableSchema>(new TableSchema(std::move(columns), keyColumn, rowStride));
    }

    TableSchema::TableSchema(vector<TableColumnDescriptor> columns, const u32 keyColumn,
                             const u32 rowStride)
        : m_Columns(std::move(columns)), m_KeyColumn(keyColumn), m_RowStride(rowStride)
    {
        VE_ASSERT(m_KeyColumn < m_Columns.size(), "TableSchema: key column {} is out of range",
                  m_KeyColumn);
    }

    const TableColumnDescriptor* TableSchema::FindColumn(const std::string_view name) const
    {
        const auto it = std::ranges::find(m_Columns, name, &TableColumnDescriptor::Name);
        return it == m_Columns.end() ? nullptr : &*it;
    }

    Ref<DataTable> DataTable::Create(AssetHandle<TableSchema> schema, const TableColumnKind keyKind,
                                     const u32 rowStride, vector<u8> rows,
                                     vector<CookedTableKey> keys, vector<u8> stringHeap)
    {
        return Ref<DataTable>(new DataTable(std::move(schema), keyKind, rowStride, std::move(rows),
                                            std::move(keys), std::move(stringHeap)));
    }

    DataTable::DataTable(AssetHandle<TableSchema> schema, const TableColumnKind keyKind,
                         const u32 rowStride, vector<u8> rows, vector<CookedTableKey> keys,
                         vector<u8> stringHeap)
        : m_Schema(std::move(schema)), m_KeyKind(keyKind), m_RowStride(rowStride),
          m_Rows(std::move(rows)), m_Keys(std::move(keys)), m_StringHeap(std::move(stringHeap))
    {
    }

    const TableSchema& DataTable::GetSchema() const
    {
        // AssetHandle exposes only operator->, which already asserts residency.
        return *m_Schema.operator->();
    }

    std::span<const u8> DataTable::GetRowBytes(const u32 row) const
    {
        VE_ASSERT(row < GetRowCount(), "DataTable: row {} is out of range ({} rows)", row,
                  GetRowCount());
        return std::span<const u8>(m_Rows).subspan(static_cast<usize>(row) * m_RowStride,
                                                   m_RowStride);
    }

    std::string_view DataTable::GetString(const CookedTableStringSpan span) const
    {
        if (static_cast<usize>(span.Offset) + span.Length > m_StringHeap.size())
        {
            return {};
        }
        return std::string_view(reinterpret_cast<const char*>(m_StringHeap.data()) + span.Offset,
                                span.Length);
    }

    optional<u32> DataTable::FindRow(const i64 key) const
    {
        VE_ASSERT(m_KeyKind == TableColumnKind::Int,
                  "DataTable: integer key lookup on a string-keyed table");

        const auto it = std::ranges::lower_bound(m_Keys, key, {}, &CookedTableKey::IntKey);
        if (it == m_Keys.end() || it->IntKey != key)
        {
            return std::nullopt;
        }
        return it->RowIndex;
    }

    optional<u32> DataTable::FindRow(const std::string_view key) const
    {
        VE_ASSERT(m_KeyKind == TableColumnKind::String,
                  "DataTable: string key lookup on an integer-keyed table");

        const auto it = std::ranges::lower_bound(
            m_Keys, key, {}, [this](const CookedTableKey& entry) -> std::string_view
            { return GetString(entry.StringKey); });
        if (it == m_Keys.end() || GetString(it->StringKey) != key)
        {
            return std::nullopt;
        }
        return it->RowIndex;
    }
}
