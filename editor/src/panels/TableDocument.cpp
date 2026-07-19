#include "panels/TableDocument.h"

#include <Veng/Asset/HexId.h>
#include <Veng/Reflection/JsonSerialize.h>

#include <algorithm>

#include <fmt/format.h>

namespace VengEditor
{
    using namespace Veng;

    namespace
    {
        // A column is a reflected field whose owning record is the row, so it addresses its value
        // at offset zero — the same descriptor the importer synthesizes per cell.
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

    // --- TableSchemaDocument ------------------------------------------------

    TableSchemaDocument TableSchemaDocument::Read(const nlohmann::json& doc,
                                                  const TypeRegistry& types)
    {
        TableSchemaDocument document;

        if (doc.contains("columns") && doc["columns"].is_array())
        {
            for (const nlohmann::json& columnJson : doc["columns"])
            {
                if (!columnJson.is_object())
                {
                    continue;
                }

                TableSchemaColumn column;
                column.Source = columnJson;
                if (columnJson.contains("name") && columnJson["name"].is_string())
                {
                    column.Name = columnJson["name"].get<string>();
                }
                if (columnJson.contains("type") && columnJson["type"].is_string())
                {
                    column.TypeName = columnJson["type"].get<string>();
                    if (const TypeInfo* const info = FindTypeByName(types, column.TypeName))
                    {
                        column.Type = info->Id;
                    }
                }
                document.Columns.push_back(std::move(column));
            }
        }

        if (doc.contains("key") && doc["key"].is_string())
        {
            document.Key = doc["key"].get<string>();
        }

        return document;
    }

    void TableSchemaDocument::Write(nlohmann::json& into) const
    {
        if (!into.is_object())
        {
            into = nlohmann::json::object();
        }

        nlohmann::json columns = nlohmann::json::array();
        for (const TableSchemaColumn& column : Columns)
        {
            nlohmann::json entry =
                column.Source.is_object() ? column.Source : nlohmann::json::object();
            entry["name"] = column.Name;
            entry["type"] = column.TypeName;
            columns.push_back(std::move(entry));
        }
        into["columns"] = std::move(columns);
        into["key"] = Key;
    }

    usize TableSchemaDocument::AddColumn(const std::string_view name, const TypeInfo& type)
    {
        string unique{name};
        for (u32 suffix = 2; FindColumn(unique); ++suffix)
        {
            unique = fmt::format("{}{}", name, suffix);
        }

        Columns.push_back(TableSchemaColumn{
            .Name = std::move(unique), .TypeName = type.QualifiedName, .Type = type.Id});
        return Columns.size() - 1;
    }

    void TableSchemaDocument::RemoveColumn(const usize index)
    {
        if (Columns[index].Name == Key)
        {
            Key.clear();
        }
        Columns.erase(Columns.begin() + static_cast<isize>(index));
    }

    void TableSchemaDocument::RenameColumn(const usize index, const std::string_view name)
    {
        const bool wasKey = Columns[index].Name == Key;
        Columns[index].Name = string{name};
        if (wasKey)
        {
            Key = Columns[index].Name;
        }
    }

    void TableSchemaDocument::SetColumnType(const usize index, const TypeInfo& type)
    {
        Columns[index].TypeName = type.QualifiedName;
        Columns[index].Type = type.Id;
    }

    void TableSchemaDocument::MoveColumn(const usize from, const usize to)
    {
        if (from == to || from >= Columns.size() || to >= Columns.size())
        {
            return;
        }

        TableSchemaColumn moved = std::move(Columns[from]);
        Columns.erase(Columns.begin() + static_cast<isize>(from));
        Columns.insert(Columns.begin() + static_cast<isize>(to), std::move(moved));
    }

    optional<usize> TableSchemaDocument::FindColumn(const std::string_view name) const
    {
        for (usize i = 0; i < Columns.size(); ++i)
        {
            if (Columns[i].Name == name)
            {
                return i;
            }
        }
        return std::nullopt;
    }

    Result<TableSchemaLayout>
    TableSchemaDocument::Resolve(const TypeRegistry& types,
                                 vector<TableColumnDescriptor>& columns) const
    {
        columns.clear();
        columns.reserve(Columns.size());
        for (const TableSchemaColumn& column : Columns)
        {
            // Reported here rather than left to the layout: the layout knows only the unresolved
            // id, and the authored spelling is what the user has to correct.
            if (column.Type == InvalidTypeId)
            {
                return std::unexpected(
                    fmt::format("column '{}': no reflected type named '{}' is registered",
                                column.Name, column.TypeName));
            }
            columns.push_back(TableColumnDescriptor{.Name = column.Name, .Type = column.Type});
        }

        return LayOutTableSchema(columns, Key, types);
    }

    // --- TableRow -----------------------------------------------------------

    TableRow::TableRow(const std::span<const TableColumnDescriptor> columns,
                       const TypeRegistry& types)
    {
        m_Cells.reserve(columns.size());
        for (const TableColumnDescriptor& column : columns)
        {
            m_Cells.push_back(CreateUnique<ReflectedStorage>(types.Info(column.Type)));
        }
    }

    // --- TableDataDocument --------------------------------------------------

    TableDataDocument TableDataDocument::Read(const nlohmann::json& doc,
                                              const std::span<const TableColumnDescriptor> columns,
                                              const TypeRegistry& types,
                                              vector<string>& diagnostics)
    {
        TableDataDocument document;

        if (doc.contains("schema") && doc["schema"].is_string())
        {
            if (const optional<u64> parsed = ParseHexId(doc["schema"].get<string>()))
            {
                document.Schema = AssetId{*parsed};
            }
            else
            {
                diagnostics.push_back(fmt::format("'schema' is a malformed hex id '{}'",
                                                  doc["schema"].get<string>()));
            }
        }
        else
        {
            diagnostics.emplace_back("missing a hex-id-string 'schema' reference");
        }

        if (!doc.contains("rows") || !doc["rows"].is_array())
        {
            diagnostics.emplace_back("missing or invalid 'rows' array");
            return document;
        }

        u32 rowIndex = 0;
        for (const nlohmann::json& rowJson : doc["rows"])
        {
            if (!rowJson.is_object())
            {
                diagnostics.push_back(fmt::format("row {}: not an object", rowIndex));
                ++rowIndex;
                continue;
            }

            TableRow row(columns, types);
            row.Source = rowJson;

            for (usize i = 0; i < columns.size(); ++i)
            {
                const TableColumnDescriptor& column = columns[i];
                if (!rowJson.contains(column.Name))
                {
                    diagnostics.push_back(
                        fmt::format("row {}: column '{}': missing", rowIndex, column.Name));
                    continue;
                }

                const string cellPath = fmt::format("row {}: {}", rowIndex, column.Name);
                if (const VoidResult bound =
                        JsonReadFieldValue(row.Cell(i), CellDescriptor(column),
                                           rowJson[column.Name], types, {}, false, cellPath);
                    !bound)
                {
                    diagnostics.push_back(bound.error());
                }
            }

            document.Rows.push_back(std::move(row));
            ++rowIndex;
        }

        return document;
    }

    void TableDataDocument::Write(nlohmann::json& into,
                                  const std::span<const TableColumnDescriptor> columns,
                                  const TypeRegistry& types) const
    {
        if (!into.is_object())
        {
            into = nlohmann::json::object();
        }

        into["schema"] = FormatHexId(Schema.Value);

        nlohmann::json rows = nlohmann::json::array();
        for (const TableRow& row : Rows)
        {
            nlohmann::json entry = row.Source.is_object() ? row.Source : nlohmann::json::object();
            for (usize i = 0; i < columns.size(); ++i)
            {
                entry[columns[i].Name] =
                    JsonWriteFieldValue(row.Cell(i), CellDescriptor(columns[i]), types);
            }
            rows.push_back(std::move(entry));
        }
        into["rows"] = std::move(rows);
    }

    usize TableDataDocument::AddRow(const std::span<const TableColumnDescriptor> columns,
                                    const TypeRegistry& types)
    {
        Rows.emplace_back(columns, types);
        return Rows.size() - 1;
    }

    usize TableDataDocument::DuplicateRow(const usize row,
                                          const std::span<const TableColumnDescriptor> columns,
                                          const TypeRegistry& types)
    {
        TableRow copy(columns, types);
        copy.Source = Rows[row].Source;
        for (usize i = 0; i < columns.size(); ++i)
        {
            const FieldDescriptor field = CellDescriptor(columns[i]);
            const nlohmann::json value = JsonWriteFieldValue(Rows[row].Cell(i), field, types);
            (void)JsonReadFieldValue(copy.Cell(i), field, value, types);
        }

        const usize inserted = row + 1;
        Rows.insert(Rows.begin() + static_cast<isize>(inserted), std::move(copy));
        return inserted;
    }

    void TableDataDocument::RemoveRow(const usize row)
    {
        Rows.erase(Rows.begin() + static_cast<isize>(row));
    }

    void TableDataDocument::MoveRow(const usize from, const usize to)
    {
        if (from == to || from >= Rows.size() || to >= Rows.size())
        {
            return;
        }

        TableRow moved = std::move(Rows[from]);
        Rows.erase(Rows.begin() + static_cast<isize>(from));
        Rows.insert(Rows.begin() + static_cast<isize>(to), std::move(moved));
    }

    vector<bool>
    TableDataDocument::DuplicateKeys(const std::span<const TableColumnDescriptor> columns,
                                     const u32 keyColumn, const TypeRegistry& types) const
    {
        vector<bool> duplicates(Rows.size(), false);
        if (keyColumn >= columns.size())
        {
            return duplicates;
        }

        const FieldDescriptor field = CellDescriptor(columns[keyColumn]);
        vector<nlohmann::json> seen;
        seen.reserve(Rows.size());
        for (usize i = 0; i < Rows.size(); ++i)
        {
            nlohmann::json key = JsonWriteFieldValue(Rows[i].Cell(keyColumn), field, types);
            duplicates[i] = std::ranges::find(seen, key) != seen.end();
            seen.push_back(std::move(key));
        }
        return duplicates;
    }
}
