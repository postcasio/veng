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

        optional<TableColumnKind> ParseColumnKind(const string& name)
        {
            if (name == "Bool")
            {
                return TableColumnKind::Bool;
            }
            if (name == "Int")
            {
                return TableColumnKind::Int;
            }
            if (name == "Float")
            {
                return TableColumnKind::Float;
            }
            if (name == "Vec2")
            {
                return TableColumnKind::Vec2;
            }
            if (name == "Vec3")
            {
                return TableColumnKind::Vec3;
            }
            if (name == "Vec4")
            {
                return TableColumnKind::Vec4;
            }
            if (name == "String")
            {
                return TableColumnKind::String;
            }
            if (name == "AssetRef")
            {
                return TableColumnKind::AssetRef;
            }
            return std::nullopt;
        }
    }

    Result<TableSchemaSource> ParseTableSchema(const path& file, const AssetTypeRegistry& types)
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
        u32 offset = 0;
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

            if (!columnJson.contains("kind") || !columnJson["kind"].is_string())
            {
                return std::unexpected(
                    Located(file, fmt::format("column '{}' is missing a string 'kind'", name)));
            }
            const string kindName = columnJson["kind"].get<string>();
            const optional<TableColumnKind> kind = ParseColumnKind(kindName);
            if (!kind)
            {
                return std::unexpected(
                    Located(file, fmt::format("column '{}': unknown kind '{}'", name, kindName)));
            }

            AssetTypeId referencedType;
            if (*kind == TableColumnKind::AssetRef)
            {
                if (!columnJson.contains("assetType") || !columnJson["assetType"].is_string())
                {
                    return std::unexpected(Located(
                        file,
                        fmt::format("AssetRef column '{}' is missing a string 'assetType'", name)));
                }
                const string typeName = columnJson["assetType"].get<string>();
                const optional<AssetTypeId> resolved = types.FindByName(typeName);
                if (!resolved)
                {
                    return std::unexpected(
                        Located(file, fmt::format("AssetRef column '{}': unknown asset type '{}'",
                                                  name, typeName)));
                }
                referencedType = *resolved;
            }
            else if (columnJson.contains("assetType"))
            {
                return std::unexpected(Located(
                    file, fmt::format("column '{}' is kind '{}' but declares an 'assetType'", name,
                                      kindName)));
            }

            const u32 alignment = TableCellAlignment(*kind);
            offset = (offset + alignment - 1) / alignment * alignment;
            schema.Columns.push_back(TableSchemaSourceColumn{
                .Name = name, .Kind = *kind, .Offset = offset, .ReferencedType = referencedType});
            offset += TableCellSize(*kind);
        }

        // Rows are indexed by a byte stride, so pad to the widest cell alignment: every column's
        // cell then sits on its own alignment in every row, not just the first.
        schema.RowStride = (offset + 7) / 8 * 8;

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
        if (keyIt->Kind != TableColumnKind::Int && keyIt->Kind != TableColumnKind::String)
        {
            return std::unexpected(Located(
                file, fmt::format("key column '{}' must be kind 'Int' or 'String'", keyName)));
        }
        schema.KeyColumn = static_cast<u32>(std::distance(schema.Columns.begin(), keyIt));

        return schema;
    }
}
