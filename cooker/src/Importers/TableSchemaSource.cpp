#include "TableSchemaSource.h"

#include <algorithm>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/JsonFile.h>

namespace Veng::Cook
{
    namespace
    {
        string Located(const path& file, const string& reason)
        {
            return fmt::format("table schema: '{}': {}", file.string(), reason);
        }

        // Column types are named by the registry's fully-qualified spelling, the same key a
        // variant alternative's "type" tag matches against — one spelling for a reflected type in
        // authored JSON, not two.
        const TypeInfo* FindTypeByName(const TypeRegistry& types, const string& name)
        {
            for (const auto& [id, info] : types.All())
            {
                if (TypeNameMatches(info, name))
                {
                    return &info;
                }
            }
            return nullptr;
        }
    }

    Result<TableSchemaSource> ParseTableSchema(const path& file, const TypeRegistry& types)
    {
        const Result<json> docResult = ReadJsonFile(file, "table schema");
        if (!docResult)
        {
            return std::unexpected(docResult.error());
        }
        const json& doc = *docResult;

        if (!doc.contains("columns") || !doc["columns"].is_array() || doc["columns"].empty())
        {
            return std::unexpected(Located(file, "missing or empty 'columns' array"));
        }

        TableSchemaSource schema;
        u32 cursor = 0;
        bool arithmetic = true;
        bool allFixed = true;

        for (const json& columnJson : doc["columns"])
        {
            if (!columnJson.is_object())
            {
                return std::unexpected(Located(file, "each 'columns' entry must be an object"));
            }
            if (!columnJson.contains("name") || !columnJson["name"].is_string())
            {
                return std::unexpected(Located(file, "a column is missing a string 'name'"));
            }
            const string name = columnJson["name"].get<string>();
            if (name.empty())
            {
                return std::unexpected(Located(file, "a column 'name' is empty"));
            }
            if (name.size() >= ShaderNameCapacity)
            {
                return std::unexpected(
                    Located(file, fmt::format("column '{}' name exceeds {} bytes", name,
                                              ShaderNameCapacity - 1)));
            }
            if (std::ranges::any_of(schema.Columns, [&name](const TableSchemaSourceColumn& existing)
                                    { return existing.Name == name; }))
            {
                return std::unexpected(
                    Located(file, fmt::format("column '{}' is declared more than once", name)));
            }

            if (!columnJson.contains("type") || !columnJson["type"].is_string())
            {
                return std::unexpected(Located(
                    file, fmt::format("column '{}' is missing a string 'type' naming a reflected "
                                      "type",
                                      name)));
            }
            const string typeName = columnJson["type"].get<string>();
            const TypeInfo* const info = FindTypeByName(types, typeName);
            if (info == nullptr)
            {
                return std::unexpected(Located(
                    file, fmt::format("column '{}': no reflected type named '{}' is registered",
                                      name, typeName)));
            }
            if (info->Class == FieldClass::Reference)
            {
                return std::unexpected(Located(
                    file, fmt::format("column '{}': type '{}' is an intra-scene entity reference, "
                                      "which no table row can resolve",
                                      name, info->QualifiedName)));
            }

            const bool fixed = TableCellIsFixedSize(info->Class);
            allFixed = allFixed && fixed;

            u32 offset = CookedTableColumnOffsetUnresolved;
            if (arithmetic && fixed)
            {
                offset = cursor;
                cursor += TableCellEncodedSize(info->Class, *info);
            }
            else
            {
                arithmetic = false;
            }

            schema.Columns.push_back(TableSchemaSourceColumn{
                .Name = name, .Type = info->Id, .Class = info->Class, .Offset = offset});
        }

        schema.FixedStride = allFixed;
        schema.RowStride = allFixed ? cursor : 0;

        if (!doc.contains("key") || !doc["key"].is_string())
        {
            return std::unexpected(Located(file, "missing a string 'key' naming the key column"));
        }
        const string keyName = doc["key"].get<string>();
        const auto keyIt =
            std::ranges::find(schema.Columns, keyName, &TableSchemaSourceColumn::Name);
        if (keyIt == schema.Columns.end())
        {
            return std::unexpected(
                Located(file, fmt::format("key column '{}' is not declared", keyName)));
        }

        const optional<TableKeyKind> keyKind = TableKeyKindForType(keyIt->Type, keyIt->Class);
        if (!keyKind)
        {
            return std::unexpected(Located(
                file, fmt::format("key column '{}' has type '{}'; a key column must be an integer "
                                  "or a string, the only types with a total order and a stable "
                                  "cooked encoding",
                                  keyName, types.Info(keyIt->Type).QualifiedName)));
        }
        schema.KeyKind = *keyKind;
        schema.KeyColumn = static_cast<u32>(std::distance(schema.Columns.begin(), keyIt));

        return schema;
    }
}
