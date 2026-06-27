#include "AssetBrowserPanel.h"

#include "../AssetDragPayload.h"
#include "../AssetSourceIndex.h"

#include <Veng/Asset/Archive.h>
#include <Veng/Log.h>
#include <Veng/UI/UI.h>
#include <VengEditor/PanelHost.h>

#include <algorithm>
#include <cctype>

namespace VengEditor
{
    using namespace Veng;

    namespace
    {
        const char* TypeName(AssetType type)
        {
            switch (type)
            {
            case AssetType::Raw:
                return "Raw";
            case AssetType::Texture:
                return "Texture";
            case AssetType::Mesh:
                return "Mesh";
            case AssetType::Shader:
                return "Shader";
            case AssetType::Material:
                return "Material";
            case AssetType::VertexLayout:
                return "VertexLayout";
            case AssetType::Prefab:
                return "Prefab";
            case AssetType::Level:
                return "Level";
            case AssetType::Skeleton:
                return "Skeleton";
            case AssetType::Animation:
                return "Animation";
            case AssetType::Environment:
                return "Environment";
            }
            return "Unknown";
        }

        // Short glyph drawn on an asset's type badge.
        const char* TypeGlyph(AssetType type)
        {
            switch (type)
            {
            case AssetType::Raw:
                return "RAW";
            case AssetType::Texture:
                return "TEX";
            case AssetType::Mesh:
                return "MSH";
            case AssetType::Shader:
                return "SHD";
            case AssetType::Material:
                return "MAT";
            case AssetType::VertexLayout:
                return "VTX";
            case AssetType::Prefab:
                return "PFB";
            case AssetType::Level:
                return "LVL";
            case AssetType::Skeleton:
                return "SKL";
            case AssetType::Animation:
                return "ANM";
            case AssetType::Environment:
                return "ENV";
            }
            return "?";
        }

        // Per-type badge fill, authored sRGB.
        vec4 TypeColor(AssetType type)
        {
            switch (type)
            {
            case AssetType::Texture:
                return {0.85f, 0.55f, 0.25f, 1.0f};
            case AssetType::Mesh:
                return {0.30f, 0.55f, 0.85f, 1.0f};
            case AssetType::Material:
                return {0.40f, 0.70f, 0.40f, 1.0f};
            case AssetType::Shader:
                return {0.60f, 0.45f, 0.80f, 1.0f};
            case AssetType::Prefab:
                return {0.30f, 0.70f, 0.70f, 1.0f};
            case AssetType::Level:
                return {0.75f, 0.45f, 0.55f, 1.0f};
            case AssetType::VertexLayout:
                return {0.55f, 0.55f, 0.60f, 1.0f};
            case AssetType::Skeleton:
                return {0.80f, 0.40f, 0.40f, 1.0f};
            case AssetType::Animation:
                return {0.50f, 0.65f, 0.30f, 1.0f};
            case AssetType::Environment:
                return {0.35f, 0.50f, 0.75f, 1.0f};
            case AssetType::Raw:
                return {0.50f, 0.50f, 0.50f, 1.0f};
            }
            return {0.50f, 0.50f, 0.50f, 1.0f};
        }

        const vec4 FolderColor{0.80f, 0.70f, 0.35f, 1.0f};

        // Source filename with the cooked extension(s) stripped (foo.tex.json -> foo).
        string DisplayName(const path& relativeSource)
        {
            path name = relativeSource.filename().stem();
            if (name.has_extension())
            {
                name = name.stem();
            }
            return name.string();
        }

        string ToLower(string_view text)
        {
            string lower(text);
            std::ranges::transform(lower, lower.begin(), [](unsigned char c)
                                   { return static_cast<char>(std::tolower(c)); });
            return lower;
        }

        // Truncates a label with an ellipsis so it fits one icon-grid cell.
        string Truncate(string_view text, usize maxChars)
        {
            if (text.size() <= maxChars)
            {
                return string(text);
            }
            return string(text.substr(0, maxChars - 1)) + "…";
        }
    }

    AssetBrowserPanel::AssetBrowserPanel(path packPath, const AssetSourceIndex& sources,
                                         PanelHost& host)
        : m_PackPath(std::move(packPath)), m_Sources(sources), m_Host(host)
    {
    }

    void AssetBrowserPanel::LoadTable()
    {
        m_Loaded = true;
        m_Root.Name = m_PackPath.filename().string();

        const Result<ArchiveReader> reader = ArchiveReader::Open(m_PackPath);
        if (!reader)
        {
            Log::Error("Asset browser: failed to open {}: {}", m_PackPath.string(), reader.error());
            return;
        }

        for (const ArchiveTocEntry& entry : reader->Entries())
        {
            const AssetSourceIndex::Entry* source = m_Sources.Find(entry.Id);

            AssetEntry asset{
                .Id = entry.Id,
                .Type = entry.Type,
                .Size = entry.Size,
                .Name = source != nullptr ? DisplayName(source->RelativeSource)
                                          : fmt::format("0x{:X}", entry.Id.Value),
            };

            // Descend (creating) the folder chain from the source's parent path; an asset
            // with no manifest source lands directly at the root.
            FolderNode* node = &m_Root;
            if (source != nullptr)
            {
                for (const path& part : source->RelativeSource.parent_path())
                {
                    const string name = part.string();
                    FolderNode& child = node->Children[name];
                    child.Name = name;
                    node = &child;
                }
            }
            node->Assets.push_back(std::move(asset));
        }
    }

    AssetBrowserPanel::FolderNode& AssetBrowserPanel::CurrentFolder()
    {
        FolderNode* node = &m_Root;
        for (const string& part : m_CurrentFolder)
        {
            const auto it = node->Children.find(part);
            if (it == node->Children.end())
            {
                m_CurrentFolder.clear();
                return m_Root;
            }
            node = &it->second;
        }
        return *node;
    }

    void AssetBrowserPanel::DrawTree(FolderNode& node, vector<string>& path)
    {
        UI::TreeFlags flags = UI::TreeFlags::OpenOnArrow | UI::TreeFlags::SpanAvailWidth;
        if (node.Children.empty())
        {
            flags = flags | UI::TreeFlags::Leaf;
        }
        if (path.empty())
        {
            flags = flags | UI::TreeFlags::DefaultOpen;
        }
        if (path == m_CurrentFolder)
        {
            flags = flags | UI::TreeFlags::Selected;
        }

        const string label = node.Name.empty() ? string{"root"} : node.Name;
        // The label repeats across folders; the joined path keeps the node id unique.
        string id;
        for (const string& part : path)
        {
            id += '/';
            id += part;
        }

        const auto tree = UI::TreeNode(fmt::format("{}##{}", label, id), flags);

        // OpenOnArrow makes a label click a selection (descend), the arrow an expand.
        if (UI::IsItemClicked() && !UI::IsItemToggledOpen())
        {
            m_CurrentFolder = path;
        }

        if (tree)
        {
            for (auto& [name, child] : node.Children)
            {
                path.push_back(name);
                DrawTree(child, path);
                path.pop_back();
            }
        }
    }

    void AssetBrowserPanel::DrawContent()
    {
        // Breadcrumb: the root plus each folder segment, click to jump.
        if (UI::SmallButton(
                fmt::format("{}##crumb_root", m_Root.Name.empty() ? "root" : m_Root.Name)))
        {
            m_CurrentFolder.clear();
        }
        for (usize i = 0; i < m_CurrentFolder.size(); ++i)
        {
            UI::SameLine();
            UI::Text("/");
            UI::SameLine();
            if (UI::SmallButton(fmt::format("{}##crumb{}", m_CurrentFolder[i], i)))
            {
                m_CurrentFolder.resize(i + 1);
            }
        }
        UI::Separator();

        const FolderNode& folder = CurrentFolder();
        const string filter = ToLower(m_Filter);
        const auto matches = [&](string_view name)
        { return filter.empty() || ToLower(name).find(filter) != string::npos; };

        // Selecting an asset records it; a double-click opens its editor (no-op for an
        // unregistered type). Shared by every layout.
        const auto activateAsset = [&](const AssetEntry& asset, bool clicked)
        {
            if (!clicked)
            {
                return;
            }
            m_Selected = asset.Id;
            if (UI::IsMouseDoubleClicked(UI::MouseButton::Left))
            {
                m_Host.OpenAssetEditor(asset.Type, asset.Id);
            }
        };
        // Attaches the drag payload to the item just submitted.
        const auto dragAsset = [&](const AssetEntry& asset)
        {
            if (const auto source = UI::DragDropSource())
            {
                const AssetDragPayload payload{.Id = asset.Id, .Type = asset.Type};
                UI::SetDragDropPayload(AssetPayload, &payload, sizeof(payload));
                UI::Badge(TypeGlyph(asset.Type), TypeColor(asset.Type));
                UI::SameLine();
                UI::Text(asset.Name);
            }
        };
        const auto activateFolder = [&](const string& name, bool clicked)
        {
            if (clicked && UI::IsMouseDoubleClicked(UI::MouseButton::Left))
            {
                m_CurrentFolder.push_back(name);
            }
        };

        if (auto view = UI::Child("##view"))
        {
            switch (m_ViewMode)
            {
            case ViewMode::DetailList:
            {
                for (const auto& [name, child] : folder.Children)
                {
                    if (!matches(name))
                    {
                        continue;
                    }
                    UI::Badge("DIR", FolderColor);
                    UI::SameLine();
                    activateFolder(name,
                                   UI::Selectable(fmt::format("{}##folder_{}", name, name), false,
                                                  UI::SelectableFlags::AllowDoubleClick));
                }
                for (const AssetEntry& asset : folder.Assets)
                {
                    if (!matches(asset.Name))
                    {
                        continue;
                    }
                    const bool selected = m_Selected && m_Selected->Value == asset.Id.Value;
                    UI::Badge(TypeGlyph(asset.Type), TypeColor(asset.Type));
                    UI::SameLine();
                    const bool clicked =
                        UI::Selectable(fmt::format("{}##asset_{}", asset.Name, asset.Id.Value),
                                       selected, UI::SelectableFlags::AllowDoubleClick);
                    activateAsset(asset, clicked);
                    dragAsset(asset);
                }
                break;
            }
            case ViewMode::Columns:
            {
                if (auto table = UI::Table("##assets", 4))
                {
                    UI::TableSetupColumn("Name");
                    UI::TableSetupColumn("Type");
                    UI::TableSetupColumn("Size");
                    UI::TableSetupColumn("Id");
                    UI::TableHeadersRow();

                    for (const auto& [name, child] : folder.Children)
                    {
                        if (!matches(name))
                        {
                            continue;
                        }
                        UI::TableNextRow();
                        UI::TableNextColumn();
                        activateFolder(name,
                                       UI::Selectable(fmt::format("{}##frow_{}", name, name), false,
                                                      UI::SelectableFlags::SpanAllColumns |
                                                          UI::SelectableFlags::AllowDoubleClick));
                        UI::TableNextColumn();
                        UI::TextDisabled("Folder");
                        UI::TableNextColumn();
                        UI::TableNextColumn();
                    }
                    for (const AssetEntry& asset : folder.Assets)
                    {
                        if (!matches(asset.Name))
                        {
                            continue;
                        }
                        UI::TableNextRow();
                        UI::TableNextColumn();
                        const bool selected = m_Selected && m_Selected->Value == asset.Id.Value;
                        const bool clicked = UI::Selectable(
                            fmt::format("{}##arow_{}", asset.Name, asset.Id.Value), selected,
                            UI::SelectableFlags::SpanAllColumns |
                                UI::SelectableFlags::AllowDoubleClick);
                        activateAsset(asset, clicked);
                        dragAsset(asset);
                        UI::TableNextColumn();
                        UI::Text(TypeName(asset.Type));
                        UI::TableNextColumn();
                        UI::Text(fmt::format("{}", asset.Size));
                        UI::TableNextColumn();
                        UI::Text(fmt::format("0x{:X}", asset.Id.Value));
                    }
                }
                break;
            }
            case ViewMode::Icons:
            {
                constexpr f32 Cell = 92.0f;
                constexpr f32 Tile = 56.0f;
                const f32 avail = UI::ContentRegionAvail().x;
                const i32 columns = std::max(1, static_cast<i32>(avail / Cell));

                // A borderless table flows cells into a grid (TableNextColumn wraps rows),
                // so the layout needs no manual SameLine/cursor arithmetic. The drag source
                // is attached while the selectable is still the last item, before the overlay.
                const auto cell = [&](string_view id, string_view glyph, vec4 color,
                                      string_view name, bool selected,
                                      const AssetEntry* dragSource) -> bool
                {
                    UI::TableNextColumn();
                    const f32 colWidth = UI::ContentRegionAvail().x;
                    const vec2 origin = UI::CursorPos();
                    const bool clicked =
                        UI::Selectable(fmt::format("##cell_{}", id), selected, vec2{0.0f, Cell},
                                       UI::SelectableFlags::AllowDoubleClick);
                    if (dragSource != nullptr)
                    {
                        dragAsset(*dragSource);
                    }
                    // Overlay the tile + label inside the cell's rect, then the table moves on.
                    UI::SetCursorPos(vec2{origin.x + ((colWidth - Tile) * 0.5f), origin.y + 6.0f});
                    UI::Badge(glyph, color, vec2{Tile, Tile});
                    UI::SetCursorPos(vec2{origin.x + 4.0f, origin.y + Tile + 10.0f});
                    UI::Text(Truncate(name, 11));
                    return clicked;
                };

                if (auto table = UI::Table("##grid", columns))
                {
                    for (const auto& [name, child] : folder.Children)
                    {
                        if (!matches(name))
                        {
                            continue;
                        }
                        activateFolder(name, cell(fmt::format("folder_{}", name), "DIR",
                                                  FolderColor, name, false, nullptr));
                    }
                    for (const AssetEntry& asset : folder.Assets)
                    {
                        if (!matches(asset.Name))
                        {
                            continue;
                        }
                        const bool selected = m_Selected && m_Selected->Value == asset.Id.Value;
                        activateAsset(asset, cell(fmt::format("asset_{}", asset.Id.Value),
                                                  TypeGlyph(asset.Type), TypeColor(asset.Type),
                                                  asset.Name, selected, &asset));
                    }
                }
                break;
            }
            }
        }
    }

    void AssetBrowserPanel::OnUI()
    {
        if (!m_Loaded)
        {
            LoadTable();
        }

        // Toolbar: three exclusive view-mode toggles plus a fill-width search box.
        bool details = m_ViewMode == ViewMode::DetailList;
        if (UI::ToggleButton("List##view_list", details))
        {
            m_ViewMode = ViewMode::DetailList;
        }
        UI::SameLine();
        bool columns = m_ViewMode == ViewMode::Columns;
        if (UI::ToggleButton("Columns##view_columns", columns))
        {
            m_ViewMode = ViewMode::Columns;
        }
        UI::SameLine();
        bool icons = m_ViewMode == ViewMode::Icons;
        if (UI::ToggleButton("Icons##view_icons", icons))
        {
            m_ViewMode = ViewMode::Icons;
        }
        UI::SameLine();
        UI::SetNextItemWidth(-1.0f);
        (void)UI::InputTextWithHint("##search", "Search", m_Filter);

        UI::Separator();

        constexpr f32 TreeWidth = 220.0f;
        if (auto tree = UI::Child("##tree", {TreeWidth, 0.0f}))
        {
            vector<string> path;
            DrawTree(m_Root, path);
        }
        UI::SameLine();
        if (auto content = UI::Child("##content"))
        {
            DrawContent();
        }
    }
}
