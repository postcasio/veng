#include "AssetBrowserPanel.h"

#include <Veng/Asset/Archive.h>
#include <Veng/Log.h>
#include <Veng/UI/UI.h>
#include <VengEditor/EditorRegistry.h>

// Raw mouse query; key/mouse input converges on the event/input system.
#include <imgui.h>

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

        UI::Text(m_PackPath.filename().string());
        UI::Separator();

        if (auto list = UI::Child("Assets"))
        {
            for (const Asset& asset : m_Assets)
            {
                const bool selected = m_Selected && m_Selected->Value == asset.Id.Value;
                const string label = fmt::format("0x{:X}  {}  {}",
                                                 asset.Id.Value, TypeName(asset.Type), asset.Size);

                if (UI::Selectable(label, selected))
                {
                    m_Selected = asset.Id;

                    // Raw mouse query; key/mouse input converges on the event/input system.
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        // The opened panel is queued for the host to adopt into its
                        // panel set on the next frame (TakeOpenedPanels).
                        if (AssetEditorFactory* factory = m_Editors.AssetEditorFor(asset.Type))
                            if (Unique<EditorPanel> panel = factory->OpenEditor(asset.Id))
                                m_Opened.push_back(std::move(panel));
                    }
                }
            }
        }
    }

    vector<Unique<EditorPanel>> AssetBrowserPanel::TakeOpenedPanels()
    {
        return std::move(m_Opened);
    }
}
