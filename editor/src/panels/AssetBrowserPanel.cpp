#include "AssetBrowserPanel.h"

#include <Veng/Asset/Archive.h>
#include <Veng/Log.h>
#include <Veng/Vendor/ImGui.h>
#include <VengEditor/EditorRegistry.h>

namespace VengEditor
{
    using namespace Veng;

    namespace
    {
        const char* TypeName(AssetType type)
        {
            switch (type)
            {
            case AssetType::Raw: return "Raw";
            case AssetType::Texture: return "Texture";
            case AssetType::Mesh: return "Mesh";
            case AssetType::Shader: return "Shader";
            case AssetType::Material: return "Material";
            case AssetType::VertexLayout: return "VertexLayout";
            case AssetType::Prefab: return "Prefab";
            }
            return "Unknown";
        }
    }

    AssetBrowserPanel::AssetBrowserPanel(path packPath, EditorRegistry& editors) :
        m_PackPath(std::move(packPath)), m_Editors(editors)
    {
    }

    void AssetBrowserPanel::LoadTable()
    {
        m_Loaded = true;

        const Result<ArchiveReader> reader = ArchiveReader::Open(m_PackPath);
        if (!reader)
        {
            Log::Error("Asset browser: failed to open {}: {}", m_PackPath.string(), reader.error());
            return;
        }

        for (const ArchiveTocEntry& entry : reader->Entries())
            m_Assets.push_back({.Id = entry.Id, .Type = entry.Type, .Size = entry.Size});
    }

    void AssetBrowserPanel::OnImGui()
    {
        if (!m_Loaded)
            LoadTable();

        ImGui::TextUnformatted(m_PackPath.filename().string().c_str());
        ImGui::Separator();

        if (ImGui::BeginTable("Assets", 3,
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
        {
            ImGui::TableSetupColumn("Id");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Size");
            ImGui::TableHeadersRow();

            for (const Asset& asset : m_Assets)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);

                const bool selected = m_Selected && m_Selected->Value == asset.Id.Value;
                char label[32];
                std::snprintf(label, sizeof(label), "0x%llX",
                              static_cast<unsigned long long>(asset.Id.Value));

                if (ImGui::Selectable(label, selected,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
                {
                    m_Selected = asset.Id;

                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        // The opened panel is queued for the host to adopt into its
                        // panel set on the next frame (TakeOpenedPanels).
                        if (AssetEditorFactory* factory = m_Editors.AssetEditorFor(asset.Type))
                            if (Unique<EditorPanel> panel = factory->OpenEditor(asset.Id))
                                m_Opened.push_back(std::move(panel));
                    }
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(TypeName(asset.Type));

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%llu", static_cast<unsigned long long>(asset.Size));
            }

            ImGui::EndTable();
        }
    }

    vector<Unique<EditorPanel>> AssetBrowserPanel::TakeOpenedPanels()
    {
        return std::move(m_Opened);
    }
}
