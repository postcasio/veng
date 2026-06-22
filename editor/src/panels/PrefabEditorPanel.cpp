#include "panels/PrefabEditorPanel.h"

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSimulation.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/InputRouter.h>
#include <Veng/Time.h>
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
                                         const AssetSourceIndex& sources, Input& input,
                                         InputRouter& router, SystemRegistry& systems)
        : PrefabEditorPanel(id, fmt::format("Prefab 0x{:X}", id.Value), context, assets, imgui,
                            types, editors, sources, input, router, systems)
    {
        AddSceneEditingChildren(context, imgui, editors, sources);
    }

    PrefabEditorPanel::PrefabEditorPanel(AssetId worldPrefab, string title,
                                         Renderer::Context& context, AssetManager& assets,
                                         ImGuiLayer& imgui, TypeRegistry& types,
                                         EditorRegistry& /*editors*/,
                                         const AssetSourceIndex& /*sources*/, Input& input,
                                         InputRouter& router, SystemRegistry& systems)
        : m_Id(worldPrefab), m_Title(std::move(title)), m_Assets(assets), m_Input(input),
          m_Router(router), m_Systems(systems)
    {
        m_Scene = Scene::Create(types);
        m_Context.Scene = m_Scene.get();
        m_Context.Assets = &assets;

        BuildScene(context, assets);
    }

    void PrefabEditorPanel::AddSceneEditingChildren(Renderer::Context& context, ImGuiLayer& imgui,
                                                    EditorRegistry& editors,
                                                    const AssetSourceIndex& sources)
    {
        auto viewport = CreateUnique<SceneViewportPanel>(context, m_Assets, imgui, m_Context,
                                                         m_Input, m_Router, *this);
        m_Viewport = viewport.get();
        auto explorer = CreateUnique<PrefabExplorerPanel>(m_Context);
        auto inspector = CreateUnique<InspectorPanel>(m_Assets, editors, sources, m_Context);

        m_ViewportChild = AddChild(std::move(viewport));
        m_ExplorerChild = AddChild(std::move(explorer));
        m_InspectorChild = AddChild(std::move(inspector));
    }

    PrefabEditorPanel::~PrefabEditorPanel()
    {
        // Children (which hold m_Context and m_Scene by reference) are released by the
        // base before the scenes, simulation, and prefab handle drop here.
        m_Simulation.reset();
        m_PlayScene.reset();
        m_Scene.reset();
        m_Prefab = {};
    }

    void PrefabEditorPanel::Play()
    {
        if (m_Context.IsPlaying() || m_Scene == nullptr)
        {
            return;
        }

        // Run the systems over an independent clone so the authored scene is never
        // mutated; the selection's handles index the edit scene, so drop it.
        m_PlayScene = m_Scene->Clone();
        m_Context.Clear();
        m_Context.Scene = m_PlayScene.get();
        m_Context.Play = PlayState::Playing;

        if (m_Simulation == nullptr)
        {
            // A prefab document runs every registered system; a level document runs the
            // ordered set GetPlaySystems() names, so Play matches exactly what the level
            // authored.
            const vector<SystemId>* playSystems = GetPlaySystems();
            m_Simulation = playSystems != nullptr
                               ? CreateUnique<SceneSimulation>(m_Systems, *playSystems)
                               : CreateUnique<SceneSimulation>(m_Systems);
        }
        m_Simulation->Start(*m_PlayScene, SystemContext{.Assets = m_Assets, .Input = m_Input});

        // The running game owns input: capture the cursor in the viewport until the release
        // chord (or window-focus loss) pops it.
        CaptureForPlay();
    }

    void PrefabEditorPanel::Stop()
    {
        if (!m_Context.IsPlaying())
        {
            return;
        }

        if (m_Simulation != nullptr && m_PlayScene != nullptr)
        {
            m_Simulation->Stop(*m_PlayScene, SystemContext{.Assets = m_Assets, .Input = m_Input});
        }

        ReleaseFromPlay();
        m_Context.Clear();
        m_PlayScene.reset();
        m_Context.Scene = m_Scene.get();
        m_Context.Play = PlayState::Editing;
    }

    void PrefabEditorPanel::Pause()
    {
        if (m_Context.Play == PlayState::Playing)
        {
            m_Context.Play = PlayState::Paused;
            // A paused game is not consuming input; free the cursor for editor interaction.
            ReleaseFromPlay();
        }
    }

    void PrefabEditorPanel::Resume()
    {
        if (m_Context.Play == PlayState::Paused)
        {
            m_Context.Play = PlayState::Playing;
            CaptureForPlay();
        }
    }

    void PrefabEditorPanel::CaptureForPlay()
    {
        if (!m_Router.IsGameplayFocused())
        {
            m_Router.PushFocus(InputFocus::Gameplay);
        }
    }

    void PrefabEditorPanel::ReleaseFromPlay()
    {
        if (m_Router.IsGameplayFocused())
        {
            m_Router.PopFocus();
        }
    }

    void PrefabEditorPanel::OnRender(Renderer::CommandBuffer& cmd)
    {
        if (m_Context.Play == PlayState::Playing && m_PlayScene != nullptr &&
            m_Simulation != nullptr)
        {
            m_Simulation->Update(*m_PlayScene, Time::GetDeltaTime(),
                                 SystemContext{.Assets = m_Assets, .Input = m_Input});
        }

        // Forward to the children after the tick so the viewport renders this frame's
        // advanced scene.
        AssetEditorPanel::OnRender(cmd);
    }

    void PrefabEditorPanel::OnUI()
    {
        if (m_Context.Scene == nullptr)
        {
            return;
        }
        UI::TextDisabled(fmt::format("{} entities", m_Context.Scene->EntityCount()));
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
