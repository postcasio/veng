#include "panels/TableSchemaEditorPanel.h"

#include "EditorIcons.h"
#include "JsonUtil.h"

#include <Veng/Asset/AssetType.h>
#include <Veng/Log.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/UI/UI.h>

#include <algorithm>
#include <fstream>

namespace VengEditor
{
    using namespace Veng;

    namespace
    {
        constexpr vec4 ErrorColor{0.9f, 0.3f, 0.3f, 1.0f};
        constexpr vec4 WarnColor{0.9f, 0.75f, 0.3f, 1.0f};

        // A column may be any reflected type except an intra-scene entity reference, which no row
        // could resolve. The picker offers exactly what LayOutTableSchema accepts.
        bool IsColumnType(const TypeInfo& info)
        {
            return info.Class != FieldClass::Reference;
        }

        bool MatchesFilter(const TypeInfo& info, const string& filter)
        {
            if (filter.empty())
            {
                return true;
            }
            return info.QualifiedName.find(filter) != string::npos;
        }
    }

    TableSchemaEditorPanel::TableSchemaEditorPanel(const AssetId id, path sourcePath,
                                                   const TypeRegistry& types, CookDriver cook)
        : m_Id(id), m_SourcePath(std::move(sourcePath)), m_Types(types), m_Cook(std::move(cook))
    {
        m_Title = fmt::format("Table Schema: {}", m_SourcePath.filename().string());
        LoadDocument();
        TriggerCook();
    }

    void TableSchemaEditorPanel::LoadDocument()
    {
        const optional<nlohmann::json> doc = ReadJsonObject(m_SourcePath);
        if (!doc)
        {
            Log::Error("Table schema editor: failed to read {}", m_SourcePath.string());
            m_Document = TableSchemaDocument{};
        }
        else
        {
            m_Document = TableSchemaDocument::Read(*doc, m_Types);
        }

        m_Dirty = false;
        m_Destructive = false;
        Revalidate();
    }

    void TableSchemaEditorPanel::Revalidate()
    {
        const Result<TableSchemaLayout> layout = m_Document.Resolve(m_Types, m_Resolved);
        if (layout)
        {
            m_Layout = *layout;
            m_ValidationError.clear();
        }
        else
        {
            m_Layout.reset();
            m_Resolved.clear();
            m_ValidationError = layout.error();
        }
    }

    VoidResult TableSchemaEditorPanel::Save()
    {
        // Round-trip the on-disk document so unknown keys survive; only columns and key are ours.
        nlohmann::json doc = ReadJsonObject(m_SourcePath).value_or(nlohmann::json::object());
        m_Document.Write(doc);

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
        m_Destructive = false;

        // The cook reads the file just written, so it runs after the write and never before it: a
        // failed cook leaves the saved source on disk and reports here.
        TriggerCook();
        return {};
    }

    void TableSchemaEditorPanel::TriggerCook()
    {
        // A cook already in flight read an older source, so a save landing now must re-cook once
        // that one lands — dropping it would leave the mounted asset behind the saved file.
        if (m_Cooking)
        {
            m_CookQueued = true;
            return;
        }

        m_Cooking = true;
        m_CookError.reset();

        m_Cook({.SourcePath = m_SourcePath, .TargetId = m_Id, .Type = AssetTypes::TableSchema},
               [this](Result<MountHandle> mount)
               {
                   m_Cooking = false;
                   if (mount)
                   {
                       m_Mount = std::move(*mount);
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

    const TypeInfo* TableSchemaEditorPanel::DrawTypePicker(const string_view id, string& filter)
    {
        auto popup = UI::Popup(id);
        if (!popup)
        {
            return nullptr;
        }

        UI::SetNextItemWidth(240.0f);
        (void)UI::InputTextWithHint("##typefilter", "Search types", filter);

        const TypeInfo* chosen = nullptr;
        // Sorted so the list is stable frame to frame: registry iteration order is unspecified.
        vector<const TypeInfo*> candidates;
        for (const auto& [typeId, info] : m_Types.All())
        {
            if (IsColumnType(info) && MatchesFilter(info, filter))
            {
                candidates.push_back(&info);
            }
        }
        std::ranges::sort(candidates, {}, [](const TypeInfo* info) { return info->QualifiedName; });

        if (auto list = UI::Child("##typelist", vec2{240.0f, 260.0f}))
        {
            for (const TypeInfo* info : candidates)
            {
                if (UI::Selectable(info->QualifiedName))
                {
                    chosen = info;
                    UI::CloseCurrentPopup();
                }
            }
        }
        return chosen;
    }

    bool TableSchemaEditorPanel::DrawKeyColumn()
    {
        // Only a column whose type has a total order and a stable cooked encoding can key rows.
        vector<string_view> labels;
        vector<usize> indices;
        labels.emplace_back("(none)");
        i32 current = 0;
        for (usize i = 0; i < m_Document.Columns.size(); ++i)
        {
            const TableSchemaColumn& column = m_Document.Columns[i];
            if (column.Type == InvalidTypeId)
            {
                continue;
            }
            const TypeInfo& info = m_Types.Info(column.Type);
            if (!TableKeyKindForType(column.Type, info.Class))
            {
                continue;
            }
            if (column.Name == m_Document.Key)
            {
                current = static_cast<i32>(labels.size());
            }
            labels.push_back(column.Name);
            indices.push_back(i);
        }

        UI::PropertyLabel("Key column");
        if (!UI::Combo("##key", current, labels))
        {
            return false;
        }

        m_Document.Key = current == 0
                             ? string{}
                             : m_Document.Columns[indices[static_cast<usize>(current) - 1]].Name;
        return true;
    }

    bool TableSchemaEditorPanel::DrawColumns()
    {
        bool changed = false;
        optional<usize> removed;
        optional<std::pair<usize, usize>> moved;

        if (auto table = UI::Table("##columns", 4))
        {
            UI::TableSetupColumn("Name");
            UI::TableSetupColumn("Type");
            UI::TableSetupColumn("Offset");
            UI::TableSetupColumn("");
            UI::TableHeadersRow();

            for (usize i = 0; i < m_Document.Columns.size(); ++i)
            {
                const TableSchemaColumn& column = m_Document.Columns[i];
                auto rowId = UI::PushId(fmt::format("col{}", i));

                UI::TableNextRow();

                UI::TableNextColumn();
                string name = column.Name;
                UI::SetNextItemWidth(-1.0f);
                if (UI::InputText("##name", name))
                {
                    m_Document.RenameColumn(i, name);
                    changed = true;
                }

                UI::TableNextColumn();
                const bool unresolved = column.Type == InvalidTypeId;
                const string typeLabel =
                    column.TypeName.empty() ? string{"(unset)"} : column.TypeName;
                if (unresolved)
                {
                    UI::TextColored(ErrorColor, Icons::Warning);
                    UI::SameLine();
                }
                if (UI::Button(fmt::format("{}##type", typeLabel)))
                {
                    m_RetypeColumn = i;
                    m_RetypeFilter.clear();
                    UI::OpenPopup("##retype");
                }
                UI::Tooltip("Pick the column's reflected type");

                UI::TableNextColumn();
                // The cooked cell offset, shown so the row layout an edit produces is legible. A
                // cell past the first variable-size column has none — it is reached by walking.
                if (i < m_Resolved.size() &&
                    m_Resolved[i].Offset != CookedTableColumnOffsetUnresolved)
                {
                    UI::TextDisabled(fmt::format("{}", m_Resolved[i].Offset));
                }
                else
                {
                    UI::TextDisabled("walked");
                }

                UI::TableNextColumn();
                if (UI::IconButton(Icons::MoveUp) && i > 0)
                {
                    moved = std::pair{i, i - 1};
                }
                UI::SameLine();
                if (UI::IconButton(Icons::MoveDown) && i + 1 < m_Document.Columns.size())
                {
                    moved = std::pair{i, i + 1};
                }
                UI::SameLine();
                if (UI::IconButton(Icons::Remove))
                {
                    removed = i;
                }
                UI::Tooltip("Remove the column");
            }
        }

        if (const TypeInfo* picked = DrawTypePicker("##retype", m_RetypeFilter))
        {
            m_Document.SetColumnType(m_RetypeColumn, *picked);
            m_Destructive = true;
            changed = true;
        }

        if (moved)
        {
            m_Document.MoveColumn(moved->first, moved->second);
            changed = true;
        }
        if (removed)
        {
            m_Document.RemoveColumn(*removed);
            m_Destructive = true;
            changed = true;
        }

        if (UI::Button(fmt::format("{} Add column", Icons::Add)))
        {
            m_AddFilter.clear();
            UI::OpenPopup("##addcolumn");
        }
        if (const TypeInfo* picked = DrawTypePicker("##addcolumn", m_AddFilter))
        {
            (void)m_Document.AddColumn("column", *picked);
            changed = true;
        }

        return changed;
    }

    void TableSchemaEditorPanel::OnUI()
    {
        if (auto bar = UI::Toolbar("##schema-toolbar"))
        {
            {
                const UI::DisabledScope disabled = UI::Disabled(!m_Dirty);
                if (UI::IconButton(Icons::Save))
                {
                    if (const VoidResult saved = Save(); !saved)
                    {
                        m_CookError = saved.error();
                        Log::Error("Table schema editor: {}", saved.error());
                    }
                }
            }
            UI::Tooltip("Save the schema to its .tableschema.json and recook");
            UI::SameLine();
            if (UI::IconButton(Icons::Revert))
            {
                LoadDocument();
            }
            UI::Tooltip("Discard edits and reload the schema from disk");
        }

        if (m_Cooking)
        {
            UI::Text("Cooking...");
        }
        if (m_CookError)
        {
            UI::TextColored(ErrorColor, fmt::format("Cook error: {}", *m_CookError));
        }
        if (!m_ValidationError.empty())
        {
            UI::TextColored(ErrorColor, fmt::format("Invalid schema: {}", m_ValidationError));
        }
        else if (m_Layout)
        {
            UI::TextDisabled(m_Layout->FixedStride
                                 ? fmt::format("Fixed-stride rows of {} bytes", m_Layout->RowStride)
                                 : string{"Variable-size rows, addressed by row directory"});
        }
        if (m_Destructive)
        {
            UI::TextColored(WarnColor,
                            fmt::format("{} A removed or retyped column invalidates every table "
                                        "cooked against this schema.",
                                        Icons::Warning));
        }

        UI::Separator();

        bool changed = DrawColumns();

        UI::Separator();
        if (auto table = UI::PropertyTable("##schema-key"))
        {
            changed = DrawKeyColumn() || changed;
        }

        if (changed)
        {
            m_Dirty = true;
            Revalidate();
        }
    }
}
