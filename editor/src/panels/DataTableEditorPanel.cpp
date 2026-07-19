#include "panels/DataTableEditorPanel.h"

#include "AssetSourceIndex.h"
#include "EditorIcons.h"
#include "FieldWidget.h"
#include "JsonUtil.h"

#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/HexId.h>
#include <Veng/Log.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/UI/UI.h>
#include <Veng/Vendor/ImGuiInternal.h>

#include <fstream>

namespace VengEditor
{
    using namespace Veng;

    namespace
    {
        constexpr vec4 ErrorColor{0.9f, 0.3f, 0.3f, 1.0f};

        // A column is a reflected field whose owning record is the row, addressing its value at
        // offset zero — the descriptor the inspector widget for the column's type draws against.
        FieldDescriptor CellDescriptor(const TableColumnDescriptor& column)
        {
            FieldDescriptor field;
            field.Name = column.Name;
            field.Type = column.Type;
            field.Class = column.Class;
            field.Offset = 0;
            return field;
        }

        // Struct, Variant, and Array cells expand into several inspector rows, so they cannot draw
        // inside one grid cell; they are edited in a popup holding a real property table instead.
        bool IsCompositeCell(const FieldClass cls)
        {
            return cls == FieldClass::Struct || cls == FieldClass::Variant ||
                   cls == FieldClass::Array;
        }
    }

    DataTableEditorPanel::DataTableEditorPanel(const AssetId id, path sourcePath,
                                               const AssetSourceIndex& sources,
                                               AssetManager& assets, const EditorRegistry& editors,
                                               CookDriver cook, AssetOpener openAsset)
        : m_Id(id), m_SourcePath(std::move(sourcePath)), m_Sources(sources), m_Assets(assets),
          m_Editors(editors), m_Cook(std::move(cook)), m_OpenAsset(std::move(openAsset))
    {
        m_Title = fmt::format("Data Table: {}", m_SourcePath.filename().string());
        LoadDocument();
        TriggerCook();
    }

    void DataTableEditorPanel::LoadDocument()
    {
        m_Columns.clear();
        m_Layout.reset();
        m_SchemaError.clear();
        m_Diagnostics.clear();
        m_Document = TableDataDocument{};
        m_Dirty = false;

        const optional<nlohmann::json> doc = ReadJsonObject(m_SourcePath);
        if (!doc)
        {
            m_SchemaError = fmt::format("failed to read {}", m_SourcePath.string());
            return;
        }

        AssetId schemaId;
        if (doc->contains("schema") && (*doc)["schema"].is_string())
        {
            if (const optional<u64> parsed = ParseHexId((*doc)["schema"].get<string>()))
            {
                schemaId = AssetId{*parsed};
            }
        }

        const AssetSourceIndex::Entry* const entry =
            schemaId.IsValid() ? m_Sources.Find(schemaId) : nullptr;
        if (entry == nullptr)
        {
            m_SchemaError = fmt::format("schema {} is not declared in this project's packs",
                                        FormatHexId(schemaId.Value));
            return;
        }
        if (entry->Type != AssetTypes::TableSchema)
        {
            m_SchemaError =
                fmt::format("asset {} is not a TableSchema", FormatHexId(schemaId.Value));
            return;
        }

        const TypeRegistry& types = m_Assets.GetTypeRegistry();
        const optional<nlohmann::json> schemaDoc = ReadJsonObject(entry->Source);
        if (!schemaDoc)
        {
            m_SchemaError = fmt::format("failed to read {}", entry->Source.string());
            return;
        }

        const TableSchemaDocument schema = TableSchemaDocument::Read(*schemaDoc, types);
        const Result<TableSchemaLayout> layout = schema.Resolve(types, m_Columns);
        if (!layout)
        {
            m_Columns.clear();
            m_SchemaError = layout.error();
            return;
        }
        m_Layout = *layout;

        m_Document = TableDataDocument::Read(*doc, m_Columns, types, m_Diagnostics);
    }

    VoidResult DataTableEditorPanel::Save()
    {
        if (!m_Layout)
        {
            return std::unexpected(fmt::format(
                "the schema did not resolve, so the rows cannot be written: {}", m_SchemaError));
        }

        nlohmann::json doc = ReadJsonObject(m_SourcePath).value_or(nlohmann::json::object());
        m_Document.Write(doc, m_Columns, m_Assets.GetTypeRegistry());

        std::ofstream out(m_SourcePath, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            return std::unexpected(fmt::format("failed to write {}", m_SourcePath.string()));
        }
        out << doc.dump(2) << '\n';
        out.close();
        if (!out)
        {
            return std::unexpected(fmt::format("failed to write {}", m_SourcePath.string()));
        }

        m_Dirty = false;
        TriggerCook();
        return {};
    }

    void DataTableEditorPanel::TriggerCook()
    {
        // A cook already in flight read an older source, so a save landing now must re-cook once
        // that one lands — dropping it would leave the mounted table behind the saved file.
        if (m_Cooking)
        {
            m_CookQueued = true;
            return;
        }

        m_Cooking = true;
        m_CookError.reset();

        m_Cook({.SourcePath = m_SourcePath, .TargetId = m_Id, .Type = AssetTypes::DataTable},
               [this](Result<MountHandle> mount)
               {
                   m_Cooking = false;
                   if (mount)
                   {
                       // Replace the mount and reload behind the stable handle, so a consumer
                       // already holding this table picks up the saved rows.
                       m_Mount = std::move(*mount);
                       m_Handle = m_Assets.Load<DataTable>(m_Id);
                   }
                   else
                   {
                       m_CookError = mount.error();
                   }

                   if (m_CookQueued)
                   {
                       m_CookQueued = false;
                       TriggerCook();
                   }
               });
    }

    bool DataTableEditorPanel::DrawCell(const usize row, const usize column)
    {
        const TableColumnDescriptor& descriptor = m_Columns[column];
        const FieldDescriptor field = CellDescriptor(descriptor);
        void* cell = m_Document.Rows[row].Cell(column);

        const FieldWidgetContext ctx{
            .Assets = m_Assets, .Sources = m_Sources, .Editors = m_Editors, .OwnerBase = cell};

        if (!IsCompositeCell(descriptor.Class))
        {
            UI::SetNextItemWidth(-1.0f);
            return DrawFieldValue(cell, field, ctx);
        }

        // A composite cell opens the ordinary inspector in a popup: the same DrawFieldWidget walk
        // the entity inspector runs, in a property table where its rows have somewhere to go.
        if (UI::Button(
                fmt::format("{}...##edit", m_Assets.GetTypeRegistry().Info(descriptor.Type).Name)))
        {
            UI::OpenPopup("##cell");
        }

        bool changed = false;
        if (auto popup = UI::Popup("##cell"))
        {
            if (auto table = UI::PropertyTable("##cellfields"))
            {
                changed = DrawFieldWidget(cell, field, ctx);
            }
        }
        return changed;
    }

    bool DataTableEditorPanel::DrawGrid()
    {
        bool changed = false;
        optional<usize> removed;
        optional<usize> duplicated;
        optional<std::pair<usize, usize>> moved;

        const usize columnCount = m_Columns.size();
        const i32 tableColumns = static_cast<i32>(columnCount) + 2;

        if (auto table = UI::Table("##rows", tableColumns))
        {
            UI::TableSetupColumn("#");
            for (const TableColumnDescriptor& column : m_Columns)
            {
                UI::TableSetupColumn(column.Name);
            }
            UI::TableSetupColumn("");
            UI::TableHeadersRow();

            // Virtualized: a table far past the visible rows submits only what is on screen, so
            // scrolling cost is independent of row count.
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(m_Document.Rows.size()));
            while (clipper.Step())
            {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    const auto row = static_cast<usize>(i);
                    auto rowId = UI::PushId(fmt::format("row{}", row));
                    UI::TableNextRow();

                    UI::TableNextColumn();
                    const bool duplicateKey = row < m_DuplicateKeys.size() && m_DuplicateKeys[row];
                    if (duplicateKey)
                    {
                        UI::TextColored(ErrorColor, fmt::format("{} {}", Icons::Warning, row));
                        UI::Tooltip("This row's key already appears above; the cook rejects it");
                    }
                    else
                    {
                        UI::TextDisabled(fmt::format("{}", row));
                    }

                    for (usize column = 0; column < columnCount; ++column)
                    {
                        UI::TableNextColumn();
                        auto cellId = UI::PushId(fmt::format("c{}", column));
                        changed = DrawCell(row, column) || changed;
                    }

                    UI::TableNextColumn();
                    if (UI::IconButton(Icons::Duplicate))
                    {
                        duplicated = row;
                    }
                    UI::Tooltip("Duplicate the row");
                    UI::SameLine();
                    if (UI::IconButton(Icons::MoveUp) && row > 0)
                    {
                        moved = std::pair{row, row - 1};
                    }
                    UI::SameLine();
                    if (UI::IconButton(Icons::MoveDown) && row + 1 < m_Document.Rows.size())
                    {
                        moved = std::pair{row, row + 1};
                    }
                    UI::SameLine();
                    if (UI::IconButton(Icons::Remove))
                    {
                        removed = row;
                    }
                    UI::Tooltip("Remove the row");
                }
            }
        }

        // Structural edits are queued through the clipper walk and applied after it: the clipper
        // holds indices into the row vector for the whole loop.
        const TypeRegistry& types = m_Assets.GetTypeRegistry();
        if (moved)
        {
            m_Document.MoveRow(moved->first, moved->second);
            changed = true;
        }
        if (duplicated)
        {
            (void)m_Document.DuplicateRow(*duplicated, m_Columns, types);
            changed = true;
        }
        if (removed)
        {
            m_Document.RemoveRow(*removed);
            changed = true;
        }

        if (UI::Button(fmt::format("{} Add row", Icons::Add)))
        {
            (void)m_Document.AddRow(m_Columns, types);
            changed = true;
        }

        return changed;
    }

    void DataTableEditorPanel::OnUI()
    {
        if (auto bar = UI::Toolbar("##table-toolbar"))
        {
            {
                const UI::DisabledScope disabled = UI::Disabled(!m_Dirty);
                if (UI::IconButton(Icons::Save))
                {
                    if (const VoidResult saved = Save(); !saved)
                    {
                        m_CookError = saved.error();
                        Log::Error("Data table editor: {}", saved.error());
                    }
                }
            }
            UI::Tooltip("Save the rows to their .table.json and recook");
            UI::SameLine();
            if (UI::IconButton(Icons::Revert))
            {
                LoadDocument();
                TriggerCook();
            }
            UI::Tooltip("Discard edits and reload the table from disk");
            UI::SameLine();
            {
                const UI::DisabledScope disabled = UI::Disabled(!m_Document.Schema.IsValid());
                if (UI::Button("Open schema"))
                {
                    m_OpenAsset(AssetTypes::TableSchema, m_Document.Schema);
                }
            }
            UI::Tooltip("Open the schema this table is authored against");
        }

        UI::TextDisabled(fmt::format("Schema {}", FormatHexId(m_Document.Schema.Value)));

        if (m_Cooking)
        {
            UI::Text("Cooking...");
        }
        if (m_CookError)
        {
            UI::TextColored(ErrorColor, fmt::format("Cook error: {}", *m_CookError));
        }
        if (!m_SchemaError.empty())
        {
            UI::TextColored(ErrorColor, fmt::format("Schema: {}", m_SchemaError));
            return;
        }
        for (const string& diagnostic : m_Diagnostics)
        {
            UI::TextColored(ErrorColor, diagnostic);
        }

        UI::Separator();

        const TypeRegistry& types = m_Assets.GetTypeRegistry();
        m_DuplicateKeys = m_Document.DuplicateKeys(m_Columns, m_Layout->KeyColumn, types);

        if (DrawGrid())
        {
            m_Dirty = true;
        }
    }
}
