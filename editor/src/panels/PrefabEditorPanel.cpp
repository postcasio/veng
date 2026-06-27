#include "panels/PrefabEditorPanel.h"

#include "EditorIcons.h"

#include <Veng/Application.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSimulation.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/InputRouter.h>
#include <Veng/Time.h>
#include <Veng/UI/UI.h>
#include <Veng/Vendor/ImGuiInternal.h>

#include "AssetSourceIndex.h"
#include "EditorGizmo.h"
#include "panels/InspectorPanel.h"
#include "panels/PrefabExplorerPanel.h"
#include "panels/SceneViewportPanel.h"
#include "PrefabSerialize.h"

namespace VengEditor
{
    using namespace Veng;

    PrefabEditorPanel::PrefabEditorPanel(AssetId id, Application& app, AssetManager& assets,
                                         ImGuiLayer& imgui, TypeRegistry& types,
                                         EditorRegistry& editors, const AssetSourceIndex& sources,
                                         Input& input, InputRouter& router, SystemRegistry& systems)
        : PrefabEditorPanel(id, fmt::format("Prefab 0x{:X}", id.Value), app, assets, imgui, types,
                            editors, sources, input, router, systems)
    {
        // The prefab document saves its entities back to the .prefab.json the manifest points at;
        // an unindexed id leaves the source empty, which disables Save.
        if (const AssetSourceIndex::Entry* entry = sources.Find(id))
        {
            m_PrefabSource = entry->Source;
        }

        AddSceneEditingChildren(app, imgui, editors, sources);
    }

    PrefabEditorPanel::PrefabEditorPanel(AssetId worldPrefab, string title, Application& /*app*/,
                                         AssetManager& assets, ImGuiLayer& imgui,
                                         TypeRegistry& types, EditorRegistry& /*editors*/,
                                         const AssetSourceIndex& /*sources*/, Input& input,
                                         InputRouter& router, SystemRegistry& systems)
        : m_Id(worldPrefab), m_BaseTitle(std::move(title)),
          m_TitleId(fmt::format("##doc0x{:X}", worldPrefab.Value)), m_Assets(assets),
          m_Input(input), m_Router(router), m_Systems(systems)
    {
        m_Scene = Scene::Create(types);
        m_Context.Scene = m_Scene.get();
        m_Context.Assets = &assets;

        BuildScene();
    }

    string_view PrefabEditorPanel::GetTitle() const
    {
        // Recompute each call so the host's dock-key lookup and the document window label agree.
        // The "##" suffix is the stable ImGui dock identity; only the visible label before it
        // carries the unsaved marker, so toggling the marker never re-docks the window.
        const bool dirty = m_Commands.IsDirty();
        m_DisplayTitle = fmt::format("{}{}{}", dirty ? "*" : "", m_BaseTitle, m_TitleId);
        return m_DisplayTitle;
    }

    VoidResult PrefabEditorPanel::Save()
    {
        if (m_Scene == nullptr)
        {
            return std::unexpected(string{"prefab editor: no scene to save"});
        }
        if (m_PrefabSource.empty())
        {
            return std::unexpected(string{"prefab editor: document has no source path to save to"});
        }

        const VoidResult written =
            PrefabSerialize::Save(*m_Scene, m_Scene->GetTypeRegistry(), m_PrefabSource);
        if (!written)
        {
            Log::Error("Prefab editor: save failed: {}", written.error());
            return written;
        }

        // The document's current state is now the saved state; the dirty marker clears.
        m_Commands.MarkSaved();
        return {};
    }

    void PrefabEditorPanel::AddSceneEditingChildren(Application& app, ImGuiLayer& imgui,
                                                    EditorRegistry& editors,
                                                    const AssetSourceIndex& sources)
    {
        auto viewport = CreateUnique<SceneViewportPanel>(app, m_Assets, imgui, m_Context, m_Input,
                                                         m_Router, m_Commands);
        m_Viewport = viewport.get();
        auto explorer = CreateUnique<PrefabExplorerPanel>(m_Context, m_Commands);
        auto inspector =
            CreateUnique<InspectorPanel>(m_Assets, editors, sources, m_Context, m_Commands);

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

        // Seed any document-scoped state (a level seeds its Session) before Start, so the spawn
        // rules a system set runs at OnStart see the same initialized scene the runtime does.
        SeedPlayScene(*m_PlayScene);

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

    void PrefabEditorPanel::TickPlaySimulation()
    {
        // Advance the play clone before the document body draws; the engine renders the viewport
        // at the next frame's start from the ViewState the viewport child pushes this frame, so
        // the tick and the camera carry the same one-frame latency. A subclass that overrides
        // OnUI (the level editor) must call this, or its play session spawns but never advances.
        if (m_Context.Play == PlayState::Playing && m_PlayScene != nullptr &&
            m_Simulation != nullptr)
        {
            m_Simulation->Update(*m_PlayScene, Time::GetDeltaTime(),
                                 SystemContext{.Assets = m_Assets, .Input = m_Input});
        }
    }

    void PrefabEditorPanel::DrawDocumentToolbar()
    {
        const UI::Theme& theme = UI::GetTheme();
        const bool playing = m_Context.IsPlaying();

        // Play transport: Play while editing; Stop + Pause/Resume while playing.
        {
            const UI::DisabledScope disabled = UI::Disabled(playing);
            const UI::StyleColorScope accent =
                UI::StyleColor(UI::StyleColorId::Button, theme.Accent);
            if (UI::Button(Icons::Play))
            {
                Play();
            }
        }
        UI::Tooltip("Clone the scene and run its systems (play in viewport)");

        UI::SameLine();
        {
            const UI::DisabledScope disabled = UI::Disabled(!playing);
            if (UI::Button(Icons::Stop))
            {
                Stop();
            }
            UI::Tooltip("Stop the play session and restore the edited scene");

            UI::SameLine();
            const bool paused = m_Context.Play == PlayState::Paused;
            if (UI::Button(paused ? Icons::Play : Icons::Pause))
            {
                if (paused)
                {
                    Resume();
                }
                else
                {
                    Pause();
                }
            }
            UI::Tooltip(paused ? "Resume the paused play session"
                               : "Pause the play session (hold the current frame)");
        }

        UI::SameLine();
        UI::Separator();
        UI::SameLine();

        // Gizmo-mode segment over the shared document mode (PrefabEditContext::Gizmo) every
        // viewport reads; mirrors the W/E/R keys. Disabled while playing (gizmos are an edit aid)
        // — the keys are gated the same way. The active mode's button is accent-tinted to read as
        // selected.
        const UI::DisabledScope gizmoDisabled = UI::Disabled(playing);
        const GizmoMode mode = m_Context.Gizmo;
        auto modeButton =
            [&](const string_view label, const GizmoMode target, const string_view tooltip)
        {
            const optional<UI::StyleColorScope> accent =
                mode == target ? optional<UI::StyleColorScope>{UI::StyleColor(
                                     UI::StyleColorId::Button, theme.Accent)}
                               : std::nullopt;
            if (UI::Button(label))
            {
                m_Context.Gizmo = target;
            }
            UI::Tooltip(tooltip);
        };

        modeButton(Icons::Translate, GizmoMode::Translate, "Translate (W)");
        UI::SameLine();
        modeButton(Icons::Rotate, GizmoMode::Rotate, "Rotate (E)");
        UI::SameLine();
        modeButton(Icons::Scale, GizmoMode::Scale, "Scale (R)");
    }

    void PrefabEditorPanel::OnUI()
    {
        TickPlaySimulation();

        if (m_Context.Scene == nullptr)
        {
            return;
        }

        if (auto bar = UI::Toolbar("##prefab-toolbar"))
        {
            DrawDocumentToolbar();
            UI::SameLine();
            UI::Separator();
            UI::SameLine();
            UI::TextDisabled(fmt::format("{} entities", m_Context.Scene->EntityCount()));
        }
    }

    void PrefabEditorPanel::BuildScene()
    {
        const AssetResult<AssetHandle<Prefab>> prefab = m_Assets.LoadSync<Prefab>(m_Id);
        if (!prefab.has_value())
        {
            Log::Error("Prefab editor: failed to load prefab 0x{:X}: {}", m_Id.Value,
                       prefab.error().Detail);
            return;
        }
        m_Prefab = *prefab;

        const Prefab::SpawnResult spawned = m_Prefab.Get()->SpawnInto(*m_Scene, m_Assets);
        if (!spawned.Roots.empty())
        {
            m_Context.SelectOnly(spawned.Roots[0]);
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
