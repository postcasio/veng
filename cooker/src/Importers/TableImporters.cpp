#include "TableImporters.h"

#include "TableSchemaSource.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_map>

#include <fmt/format.h>

#include <Veng/Asset/AssetHandleType.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/HexId.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Cook/JsonFile.h>
#include <Veng/Reflection/JsonSerialize.h>
#include <Veng/Reflection/Serialize.h>

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

        // A table's columns are reflected types, so a layout cannot be derived without the
        // registry. Guessing one would cook a blob that loads clean and addresses garbage.
        string MissingRegistry(const path& file, const char* what)
        {
            return fmt::format(
                "{}: '{}': cooking a table needs the reflected type registry, which this cook does "
                "not carry — pass --module so the columns' types can be resolved",
                what, file.string());
        }

        // Cook-time AssetId policy for a cell, identical to the prefab importer's: an id that
        // resolves in this pack must be of the type the handle expects. Calling Resolve is also
        // what records the cross-asset dependency, which is what keeps the recook graph correct.
        JsonFieldHooks MakeHooks(const CookContext& context)
        {
            JsonFieldHooks hooks;
            hooks.ValidateAssetId = [&context](const u64 id, const TypeId fieldType) -> VoidResult
            {
                const optional<AssetTypeId> expected =
                    context.AssetTypes->FindByHandleField(fieldType);
                if (!expected)
                {
                    // No registered mapping means the column's asset type never declared its
                    // handle leaf — a registration mistake. Skipping the check instead would
                    // let the column accept an id of any type at all.
                    return std::unexpected(fmt::format(
                        "column is an AssetHandle whose leaf type {} no registered asset type "
                        "claims; set HandleFieldType on the type's registration (and pass "
                        "--module if it is a module-defined type)",
                        FormatHexId(fieldType)));
                }

                const optional<ResolvedSource> resolved = context.Resolve(AssetId{.Value = id});
                if (!resolved)
                {
                    // Stricter than a prefab's handle field: a table is a catalogue, so a cell
                    // naming an asset the pack does not declare is an authoring mistake, not a
                    // reference the runtime might resolve some other way.
                    return std::unexpected(
                        fmt::format("asset {} is not declared in this pack or its references",
                                    FormatHexId(id)));
                }
                if (!AssetHandleFieldAccepts(*expected, resolved->Type))
                {
                    return std::unexpected(
                        fmt::format("asset {} resolves to type {} but the column expects type {}",
                                    FormatHexId(id), context.AssetTypes->GetName(resolved->Type),
                                    context.AssetTypes->GetName(*expected)));
                }
                return {};
            };
            return hooks;
        }

        // Reads a decoded integer key cell out of its reflected storage, widened to the i64 the
        // key index orders on. The type set is exactly TableKeyKindForType's integer arm.
        i64 IntegerKeyValue(const void* cell, const TypeId type)
        {
            if (type == TypeIdOf<u8>())
            {
                return *static_cast<const u8*>(cell);
            }
            if (type == TypeIdOf<i32>())
            {
                return *static_cast<const i32*>(cell);
            }
            if (type == TypeIdOf<u32>())
            {
                return *static_cast<const u32*>(cell);
            }
            return *static_cast<const i64*>(cell);
        }
    }

    Result<vector<u8>> TableSchemaImporter::Cook(const CookContext& context,
                                                 const json& entry) const
    {
        if (!entry.contains("source") || !entry["source"].is_string())
        {
            return std::unexpected("table schema: missing or invalid 'source'");
        }

        const path sourcePath = context.PackDir / entry["source"].get<string>();
        if (context.Types == nullptr)
        {
            return std::unexpected(MissingRegistry(sourcePath, "table schema"));
        }

        const Result<TableSchemaSource> schemaResult = ParseTableSchema(sourcePath, *context.Types);
        if (!schemaResult)
        {
            return std::unexpected(schemaResult.error());
        }
        const TableSchemaSource& schema = *schemaResult;

        CookedTableSchemaHeader header{};
        header.Version = CookedTableSchemaVersion;
        header.ColumnCount = static_cast<u32>(schema.Columns.size());
        header.KeyColumn = schema.KeyColumn;
        header.KeyKind = static_cast<u32>(schema.KeyKind);
        header.RowStride = schema.RowStride;
        header.FixedStride = schema.FixedStride ? 1u : 0u;

        vector<u8> blob;
        blob.reserve(sizeof(CookedTableSchemaHeader) +
                     schema.Columns.size() * sizeof(CookedTableColumn));
        Append(blob, header);
        for (const TableColumnDescriptor& column : schema.Columns)
        {
            CookedTableColumn cooked{};
            CopyName(cooked.Name, column.Name);
            cooked.Type = column.Type;
            cooked.Offset = column.Offset;
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
        if (context.Types == nullptr)
        {
            return std::unexpected(MissingRegistry(sourcePath, "data table"));
        }
        const TypeRegistry& types = *context.Types;

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
            ParseTableSchema(resolvedSchema->AbsolutePath, types);
        if (!schemaResult)
        {
            return std::unexpected(schemaResult.error());
        }
        const TableSchemaSource& schema = *schemaResult;

        // --- 2. Bind and encode the rows through the shared reflection walkers ---

        if (!doc.contains("rows") || !doc["rows"].is_array())
        {
            return std::unexpected(LocatedTable(sourcePath, "missing or invalid 'rows' array"));
        }

        const JsonFieldHooks hooks = MakeHooks(context);

        vector<u8> rows;
        vector<u32> rowOffsets;
        vector<CookedTableKey> keys;
        vector<u8> keyHeap;
        std::unordered_map<string, CookedTableStringSpan> internedKeys;

        u32 rowIndex = 0;
        for (const json& rowJson : doc["rows"])
        {
            if (!rowJson.is_object())
            {
                return std::unexpected(
                    LocatedTable(sourcePath, "each 'rows' entry must be an object"));
            }

            for (const auto& [key, unused] : rowJson.items())
            {
                if (std::ranges::none_of(schema.Columns, [&key](const TableColumnDescriptor& column)
                                         { return column.Name == key; }))
                {
                    return std::unexpected(LocatedTable(
                        sourcePath, fmt::format("row {}: no column named '{}'", rowIndex, key)));
                }
            }

            rowOffsets.push_back(static_cast<u32>(rows.size()));
            CookedTableKey keyEntry{};
            keyEntry.RowIndex = rowIndex;

            for (u32 columnIndex = 0; columnIndex < schema.Columns.size(); ++columnIndex)
            {
                const TableColumnDescriptor& column = schema.Columns[columnIndex];
                const string located = fmt::format("row {}: column '{}'", rowIndex, column.Name);
                // The bind path is the dotted prefix the walker descends from, so a malformed
                // field inside a struct or array cell is located down to that inner field.
                const string cellPath = fmt::format("row {}: {}", rowIndex, column.Name);

                if (!rowJson.contains(column.Name))
                {
                    return std::unexpected(
                        LocatedTable(sourcePath, fmt::format("{}: missing", located)));
                }

                // A cell is bound and encoded exactly as the same type would be as a struct
                // field: one JSON walker in, one record walker out, no table-specific codec.
                FieldDescriptor field;
                field.Name = column.Name;
                field.Type = column.Type;
                field.Class = column.Class;
                field.Offset = 0;

                const ReflectedStorage cell(types.Info(column.Type));
                if (const VoidResult bound = JsonReadFieldValue(
                        cell.Get(), field, rowJson[column.Name], types, hooks, false, cellPath);
                    !bound)
                {
                    return std::unexpected(LocatedTable(sourcePath, bound.error()));
                }

                if (columnIndex == schema.KeyColumn)
                {
                    if (schema.KeyKind == TableKeyKind::Integer)
                    {
                        keyEntry.IntKey = IntegerKeyValue(cell.Get(), column.Type);
                    }
                    else
                    {
                        const auto& text = *static_cast<const string*>(cell.Get());
                        const auto existing = internedKeys.find(text);
                        if (existing != internedKeys.end())
                        {
                            keyEntry.StringKey = existing->second;
                        }
                        else
                        {
                            const CookedTableStringSpan span{
                                .Offset = static_cast<u32>(keyHeap.size()),
                                .Length = static_cast<u32>(text.size())};
                            keyHeap.insert(keyHeap.end(), text.begin(), text.end());
                            internedKeys.emplace(text, span);
                            keyEntry.StringKey = span;
                        }
                    }
                }

                WriteFieldValue(rows, cell.Get(), field, types);
            }

            // Row offsets are u32, so a table whose rows cross 4 GiB cannot be addressed. Failing
            // here is the whole point: a truncated offset would cook a blob that loads clean.
            if (rows.size() > std::numeric_limits<u32>::max())
            {
                return std::unexpected(LocatedTable(
                    sourcePath,
                    fmt::format("the row region exceeds the 4 GiB a u32 row directory can address "
                                "at row {}; split the table",
                                rowIndex)));
            }

            keys.push_back(keyEntry);
            ++rowIndex;
        }

        if (schema.FixedStride && schema.RowStride != 0 &&
            rows.size() != static_cast<usize>(rowIndex) * schema.RowStride)
        {
            return std::unexpected(LocatedTable(
                sourcePath, fmt::format("encoded {} bytes for {} fixed-stride rows of {} bytes",
                                        rows.size(), rowIndex, schema.RowStride)));
        }

        // --- 3. Sort the key index and reject duplicates ---

        const auto heapString = [&keyHeap](const CookedTableStringSpan span) -> std::string_view
        {
            if (span.Length == 0)
            {
                return {};
            }
            return std::string_view(reinterpret_cast<const char*>(keyHeap.data()) + span.Offset,
                                    span.Length);
        };

        if (schema.KeyKind == TableKeyKind::Integer)
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
                schema.KeyKind == TableKeyKind::Integer
                    ? keys[i].IntKey == keys[i - 1].IntKey
                    : heapString(keys[i].StringKey) == heapString(keys[i - 1].StringKey);
            if (duplicate)
            {
                const string spelling = schema.KeyKind == TableKeyKind::Integer
                                            ? fmt::format("{}", keys[i].IntKey)
                                            : string(heapString(keys[i].StringKey));
                return std::unexpected(LocatedTable(
                    sourcePath, fmt::format("key '{}' appears in more than one row", spelling)));
            }
        }

        // --- 4. Assemble the blob ---

        CookedDataTableHeader header{};
        header.SchemaId = *schemaId;
        header.Version = CookedDataTableVersion;
        header.RowCount = rowIndex;
        header.FixedStride = schema.FixedStride ? 1u : 0u;
        header.RowStride = schema.RowStride;
        header.RowBytes = static_cast<u32>(rows.size());
        header.KeyHeapBytes = static_cast<u32>(keyHeap.size());
        header.KeyKind = static_cast<u32>(schema.KeyKind);

        // A fixed-stride table addresses rows arithmetically, so its directory is pure overhead.
        const bool writeDirectory = !schema.FixedStride;

        vector<u8> blob;
        blob.reserve(sizeof(CookedDataTableHeader) + keys.size() * sizeof(CookedTableKey) +
                     (writeDirectory ? rowOffsets.size() * sizeof(u32) : 0) + rows.size() +
                     keyHeap.size());
        Append(blob, header);
        for (const CookedTableKey& key : keys)
        {
            Append(blob, key);
        }
        if (writeDirectory)
        {
            for (const u32 offset : rowOffsets)
            {
                Append(blob, offset);
            }
        }
        blob.insert(blob.end(), rows.begin(), rows.end());
        blob.insert(blob.end(), keyHeap.begin(), keyHeap.end());

        return blob;
    }

    void RegisterTableImporters(Cooker& cooker)
    {
        cooker.Register(CreateUnique<TableSchemaImporter>());
        cooker.Register(CreateUnique<DataTableImporter>());
    }
}
