#include "TableImporters.h"

#include "TableSchemaSource.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/HexId.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Cook/JsonFile.h>

namespace Veng::Cook
{
    namespace
    {
        string LocatedTable(const path& file, const string& reason)
        {
            return fmt::format("data table: '{}': {}", file.string(), reason);
        }

        template <class T>
        void Append(vector<u8>& out, const T& value)
        {
            const auto* p = reinterpret_cast<const u8*>(&value);
            out.insert(out.end(), p, p + sizeof(T));
        }

        void CopyName(char (&destination)[ShaderNameCapacity], const string& name)
        {
            const usize length = std::min(name.size(), ShaderNameCapacity - 1);
            std::memcpy(destination, name.data(), length);
            destination[length] = '\0';
        }

        // Writes a cell of the given kind into the row record at the column's offset.
        // Located errors name the row and column; the caller supplies the row prefix.
        struct CellWriter
        {
            vector<u8>& Row;
            vector<u8>& StringHeap;
            std::unordered_map<string, CookedTableStringSpan>& InternedStrings;

            VoidResult WriteScalarArray(const json& value, const u32 offset, const u32 components,
                                        const string& located)
            {
                if (!value.is_array() || value.size() != components)
                {
                    return std::unexpected(
                        fmt::format("{}: expected an array of {} numbers", located, components));
                }
                for (u32 i = 0; i < components; ++i)
                {
                    if (!value[i].is_number())
                    {
                        return std::unexpected(
                            fmt::format("{}: component {} is not a number", located, i));
                    }
                    const auto component = value[i].get<f32>();
                    std::memcpy(Row.data() + offset + i * sizeof(f32), &component, sizeof(f32));
                }
                return {};
            }

            // Interns a string into the heap so repeated cell values cost one copy.
            CookedTableStringSpan Intern(const string& text)
            {
                const auto existing = InternedStrings.find(text);
                if (existing != InternedStrings.end())
                {
                    return existing->second;
                }
                const CookedTableStringSpan span{.Offset = static_cast<u32>(StringHeap.size()),
                                                 .Length = static_cast<u32>(text.size())};
                StringHeap.insert(StringHeap.end(), text.begin(), text.end());
                InternedStrings.emplace(text, span);
                return span;
            }
        };
    }

    Result<vector<u8>> TableSchemaImporter::Cook(const CookContext& context,
                                                 const json& entry) const
    {
        if (!entry.contains("source") || !entry["source"].is_string())
        {
            return std::unexpected("table schema: missing or invalid 'source'");
        }

        const path sourcePath = context.PackDir / entry["source"].get<string>();
        const Result<TableSchemaSource> schemaResult =
            ParseTableSchema(sourcePath, *context.AssetTypes);
        if (!schemaResult)
        {
            return std::unexpected(schemaResult.error());
        }
        const TableSchemaSource& schema = *schemaResult;

        CookedTableSchemaHeader header{};
        header.Version = CookedTableSchemaVersion;
        header.ColumnCount = static_cast<u32>(schema.Columns.size());
        header.KeyColumn = schema.KeyColumn;
        header.RowStride = schema.RowStride;

        vector<u8> blob;
        blob.reserve(sizeof(CookedTableSchemaHeader) +
                     schema.Columns.size() * sizeof(CookedTableColumn));
        Append(blob, header);
        for (const TableSchemaSourceColumn& column : schema.Columns)
        {
            CookedTableColumn cooked{};
            CopyName(cooked.Name, column.Name);
            cooked.Kind = static_cast<u32>(column.Kind);
            cooked.Offset = column.Offset;
            cooked.ReferencedType = column.ReferencedType.Value;
            Append(blob, cooked);
        }

        return blob;
    }

    Result<vector<u8>> DataTableImporter::Cook(const CookContext& context, const json& entry) const
    {
        if (!entry.contains("source") || !entry["source"].is_string())
        {
            return std::unexpected("data table: missing or invalid 'source'");
        }

        const path sourcePath = context.PackDir / entry["source"].get<string>();

        const Result<json> docResult = ReadJsonFile(sourcePath, "data table");
        if (!docResult)
        {
            return std::unexpected(docResult.error());
        }
        const json& doc = *docResult;

        // --- 1. Resolve and re-parse the schema ---

        if (!doc.contains("schema") || !doc["schema"].is_string())
        {
            return std::unexpected(
                LocatedTable(sourcePath, "missing a hex-id-string 'schema' reference"));
        }
        const optional<u64> schemaId = ParseHexId(doc["schema"].get<string>());
        if (!schemaId || *schemaId == 0)
        {
            return std::unexpected(
                LocatedTable(sourcePath, fmt::format("'schema' is a malformed or null hex id '{}'",
                                                     doc["schema"].get<string>())));
        }

        const optional<ResolvedSource> resolvedSchema = context.Resolve(AssetId{*schemaId});
        if (!resolvedSchema)
        {
            return std::unexpected(LocatedTable(
                sourcePath, fmt::format("schema {} is not declared in this pack or its references",
                                        FormatHexId(*schemaId))));
        }
        if (resolvedSchema->Type != AssetTypes::TableSchema)
        {
            return std::unexpected(LocatedTable(
                sourcePath,
                fmt::format("schema {} is a {}, not a TableSchema", FormatHexId(*schemaId),
                            context.AssetTypes->GetName(resolvedSchema->Type))));
        }

        const Result<TableSchemaSource> schemaResult =
            ParseTableSchema(resolvedSchema->AbsolutePath, *context.AssetTypes);
        if (!schemaResult)
        {
            return std::unexpected(schemaResult.error());
        }
        const TableSchemaSource& schema = *schemaResult;

        // --- 2. Validate and encode the rows ---

        if (!doc.contains("rows") || !doc["rows"].is_array())
        {
            return std::unexpected(LocatedTable(sourcePath, "missing or invalid 'rows' array"));
        }

        vector<u8> rows;
        vector<u8> stringHeap;
        vector<CookedTableKey> keys;
        std::unordered_map<string, CookedTableStringSpan> interned;

        u32 rowIndex = 0;
        for (const json& rowJson : doc["rows"])
        {
            if (!rowJson.is_object())
            {
                return std::unexpected(
                    LocatedTable(sourcePath, "each 'rows' entry must be object"));
            }

            for (const auto& [key, unused] : rowJson.items())
            {
                if (std::ranges::none_of(schema.Columns,
                                         [&key](const TableSchemaSourceColumn& column)
                                         { return column.Name == key; }))
                {
                    return std::unexpected(LocatedTable(
                        sourcePath, fmt::format("row {}: no column named '{}'", rowIndex, key)));
                }
            }

            vector<u8> record(schema.RowStride, 0);
            CookedTableKey keyEntry{};
            keyEntry.RowIndex = rowIndex;

            for (u32 columnIndex = 0; columnIndex < schema.Columns.size(); ++columnIndex)
            {
                const TableSchemaSourceColumn& column = schema.Columns[columnIndex];
                const string located = fmt::format("row {}: column '{}'", rowIndex, column.Name);

                if (!rowJson.contains(column.Name))
                {
                    return std::unexpected(
                        LocatedTable(sourcePath, fmt::format("{}: missing", located)));
                }
                const json& value = rowJson[column.Name];

                CellWriter writer{
                    .Row = record, .StringHeap = stringHeap, .InternedStrings = interned};

                switch (column.Kind)
                {
                case TableColumnKind::Bool:
                {
                    if (!value.is_boolean())
                    {
                        return std::unexpected(LocatedTable(
                            sourcePath, fmt::format("{}: expected a boolean", located)));
                    }
                    const u32 raw = value.get<bool>() ? 1u : 0u;
                    std::memcpy(record.data() + column.Offset, &raw, sizeof(raw));
                    break;
                }
                case TableColumnKind::Int:
                {
                    if (!value.is_number_integer())
                    {
                        return std::unexpected(LocatedTable(
                            sourcePath, fmt::format("{}: expected an integer", located)));
                    }
                    const auto raw = value.get<i64>();
                    std::memcpy(record.data() + column.Offset, &raw, sizeof(raw));
                    if (columnIndex == schema.KeyColumn)
                    {
                        keyEntry.IntKey = raw;
                    }
                    break;
                }
                case TableColumnKind::Float:
                {
                    if (!value.is_number())
                    {
                        return std::unexpected(LocatedTable(
                            sourcePath, fmt::format("{}: expected a number", located)));
                    }
                    const auto raw = value.get<f32>();
                    std::memcpy(record.data() + column.Offset, &raw, sizeof(raw));
                    break;
                }
                case TableColumnKind::Vec2:
                case TableColumnKind::Vec3:
                case TableColumnKind::Vec4:
                {
                    const u32 components = TableCellSize(column.Kind) / sizeof(f32);
                    const VoidResult written =
                        writer.WriteScalarArray(value, column.Offset, components, located);
                    if (!written)
                    {
                        return std::unexpected(LocatedTable(sourcePath, written.error()));
                    }
                    break;
                }
                case TableColumnKind::String:
                {
                    if (!value.is_string())
                    {
                        return std::unexpected(LocatedTable(
                            sourcePath, fmt::format("{}: expected a string", located)));
                    }
                    const CookedTableStringSpan span = writer.Intern(value.get<string>());
                    std::memcpy(record.data() + column.Offset, &span, sizeof(span));
                    if (columnIndex == schema.KeyColumn)
                    {
                        keyEntry.StringKey = span;
                    }
                    break;
                }
                case TableColumnKind::AssetRef:
                {
                    if (!value.is_string())
                    {
                        return std::unexpected(LocatedTable(
                            sourcePath, fmt::format("{}: expected a hex-id string", located)));
                    }
                    const optional<u64> referenced = ParseHexId(value.get<string>());
                    if (!referenced)
                    {
                        return std::unexpected(
                            LocatedTable(sourcePath, fmt::format("{}: malformed hex id '{}'",
                                                                 located, value.get<string>())));
                    }
                    if (*referenced != 0)
                    {
                        const optional<ResolvedSource> target =
                            context.Resolve(AssetId{*referenced});
                        if (!target)
                        {
                            return std::unexpected(LocatedTable(
                                sourcePath,
                                fmt::format("{}: asset {} is not declared in this pack or its "
                                            "references",
                                            located, FormatHexId(*referenced))));
                        }
                        if (target->Type != column.ReferencedType)
                        {
                            return std::unexpected(LocatedTable(
                                sourcePath,
                                fmt::format("{}: asset {} is a {}, but the column references {}",
                                            located, FormatHexId(*referenced),
                                            context.AssetTypes->GetName(target->Type),
                                            context.AssetTypes->GetName(column.ReferencedType))));
                        }
                    }
                    std::memcpy(record.data() + column.Offset, &*referenced, sizeof(u64));
                    break;
                }
                }
            }

            rows.insert(rows.end(), record.begin(), record.end());
            keys.push_back(keyEntry);
            ++rowIndex;
        }

        // --- 3. Sort the key index and reject duplicates ---

        const TableColumnKind keyKind = schema.Columns[schema.KeyColumn].Kind;
        const auto heapString = [&stringHeap](const CookedTableStringSpan span) -> std::string_view
        {
            if (span.Length == 0)
            {
                return {};
            }
            return std::string_view(reinterpret_cast<const char*>(stringHeap.data()) + span.Offset,
                                    span.Length);
        };

        if (keyKind == TableColumnKind::Int)
        {
            std::ranges::sort(keys, {}, &CookedTableKey::IntKey);
        }
        else
        {
            std::ranges::sort(keys, {}, [&heapString](const CookedTableKey& key)
                              { return heapString(key.StringKey); });
        }

        for (usize i = 1; i < keys.size(); ++i)
        {
            const bool duplicate =
                keyKind == TableColumnKind::Int
                    ? keys[i].IntKey == keys[i - 1].IntKey
                    : heapString(keys[i].StringKey) == heapString(keys[i - 1].StringKey);
            if (duplicate)
            {
                const string spelling = keyKind == TableColumnKind::Int
                                            ? fmt::format("{}", keys[i].IntKey)
                                            : string(heapString(keys[i].StringKey));
                return std::unexpected(LocatedTable(
                    sourcePath, fmt::format("key '{}' appears in more than one row", spelling)));
            }
        }

        // --- 4. Assemble the blob ---

        CookedDataTableHeader header{};
        header.Version = CookedDataTableVersion;
        header.RowCount = rowIndex;
        header.RowStride = schema.RowStride;
        header.StringHeapBytes = static_cast<u32>(stringHeap.size());
        header.SchemaId = *schemaId;
        header.KeyKind = static_cast<u32>(keyKind);

        vector<u8> blob;
        blob.reserve(sizeof(CookedDataTableHeader) + keys.size() * sizeof(CookedTableKey) +
                     rows.size() + stringHeap.size());
        Append(blob, header);
        for (const CookedTableKey& key : keys)
        {
            Append(blob, key);
        }
        blob.insert(blob.end(), rows.begin(), rows.end());
        blob.insert(blob.end(), stringHeap.begin(), stringHeap.end());

        return blob;
    }

    void RegisterTableImporters(Cooker& cooker)
    {
        cooker.Register(CreateUnique<TableSchemaImporter>());
        cooker.Register(CreateUnique<DataTableImporter>());
    }
}
