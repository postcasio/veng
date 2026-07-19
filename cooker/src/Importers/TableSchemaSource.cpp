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

            schema.Columns.push_back(TableColumnDescriptor{.Name = name, .Type = info->Id});
        }

        if (!doc.contains("key") || !doc["key"].is_string())
        {
            return std::unexpected(Located(file, "missing a string 'key' naming the key column"));
        }

        const Result<TableSchemaLayout> layout =
            LayOutTableSchema(schema.Columns, doc["key"].get<string>(), types);
        if (!layout)
        {
            return std::unexpected(Located(file, layout.error()));
        }

        schema.KeyColumn = layout->KeyColumn;
        schema.KeyKind = layout->KeyKind;
        schema.FixedStride = layout->FixedStride;
        schema.RowStride = layout->RowStride;

        return schema;
    }
}
