#include "panels/PrefabEditorPanel.h"

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/UI/UI.h>
#include <Veng/Vendor/ImGuiInternal.h>

#include "panels/InspectorPanel.h"
#include "panels/PrefabExplorerPanel.h"
#include "panels/SceneViewportPanel.h"

namespace VengEditor
{
    using namespace Veng;

    PrefabEditorPanel::PrefabEditorPanel(AssetId id, Renderer::Context& context,
                                         AssetManager& assets, ImGuiLayer& imgui,
                                         TypeRegistry& types, EditorRegistry& editors,
                                         const AssetSourceIndex& sources)
        : m_Id(id), m_Title(fmt::format("Prefab 0x{:X}", id.Value))
    {
        m_Scene = Scene::Create(types);
        m_Context.Scene = m_Scene.get();
        m_Context.Assets = &assets;

        BuildScene(context, assets);

        auto viewport = CreateUnique<SceneViewportPanel>(context, assets, imgui, m_Context);
        auto explorer = CreateUnique<PrefabExplorerPanel>(m_Context);
        auto inspector = CreateUnique<InspectorPanel>(assets, editors, sources, m_Context);

        m_ViewportChild = AddChild(std::move(viewport));
        m_ExplorerChild = AddChild(std::move(explorer));
        m_InspectorChild = AddChild(std::move(inspector));
    }

    PrefabEditorPanel::~PrefabEditorPanel()
    {
        // Children (which hold m_Context and m_Scene by reference) are released by the
        // base before the scene and prefab handle drop here.
        m_Scene.reset();
        m_Prefab = {};
    }

    void PrefabEditorPanel::OnImGui()
    {
        if (m_Scene == nullptr)
        {
            return;
        }
        UI::TextDisabled(fmt::format("{} entities", m_Scene->EntityCount()));
    }

    void PrefabEditorPanel::BuildScene(Renderer::Context& context, AssetManager& assets)
    {
        const AssetResult<AssetHandle<Prefab>> prefab = assets.LoadSync<Prefab>(m_Id);
        if (!prefab.has_value())
        {
            Log::Error("Prefab editor: failed to load prefab 0x{:X}: {}", m_Id.Value,
                       prefab.error().Detail);
            return;
        }
        m_Prefab = *prefab;

        const vector<Entity> roots = m_Prefab.Get()->SpawnInto(*m_Scene, assets);
        if (!roots.empty())
        {
            m_Context.SelectOnly(roots[0]);
        }

        // Light the scene when the prefab carries none, so the spawned content is visible.
        bool hasLight = false;
        m_Scene->Each<Light>([&hasLight](Entity, Light&) { hasLight = true; });
        if (!hasLight)
        {
            const Entity light = m_Scene->CreateEntity();
            m_Scene->Add<Name>(light) = Name{.Value = "Directional Light"};
            m_Scene->Add<Light>(light) = Light{
                .Type = LightType::Directional,
                .Direction = glm::normalize(vec3(-0.4f, -0.7f, -0.5f)),
                .Color = vec3(1.0f, 1.0f, 1.0f),
                .Intensity = 1.5f,
            };
        }
    }

    void PrefabEditorPanel::BuildDefaultLayout(u32 dockspaceId)
    {
        ImGuiID center = dockspaceId;
        const ImGuiID left =
            ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.22f, nullptr, &center);
        const ImGuiID right =
            ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, nullptr, &center);

        DockChildWindow(m_ExplorerChild, left);
        DockChildWindow(m_ViewportChild, center);
        DockChildWindow(m_InspectorChild, right);
    }
}
