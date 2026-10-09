#include "panels/PrefabEditorPanel.h"

#include "EditorIcons.h"

#include <Veng/Application.h>
#include <Veng/ManagedViewports.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Input.h>
#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Requests.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/InputRouter.h>
#include <Veng/UI/UI.h>
#include <Veng/Vendor/ImGuiInternal.h>

#include "AssetSourceIndex.h"
#include "EditorGizmo.h"
#include "EditorOnly.h"
#include "panels/InspectorPanel.h"
#include "panels/PrefabExplorerPanel.h"
#include "panels/SceneViewportPanel.h"
#include "PrefabSerialize.h"
#include "PreviewLook.h"

namespace VengEditor
{
    using namespace Veng;

    PrefabEditorPanel::PrefabEditorPanel(AssetId id, Application& app, AssetManager& assets,
                                         ImGuiLayer& imgui, TypeRegistry& types,
                                         EditorRegistry& editors, const AssetSourceIndex& sources,
                                         Input& input, InputRouter& router, SystemRegistry& systems,
                                         const PreviewLook& look)
        : PrefabEditorPanel(id, fmt::format("Prefab 0x{:X}", id.Value), app, assets, imgui, types,
                            editors, sources, input, router, systems, &look)
    {
        // The prefab document saves its entities back to the .prefab.json the manifest points at;
        // an unindexed id leaves the source empty, which disables Save.
        if (const AssetSourceIndex::Entry* entry = sources.Find(id))
        {
            m_PrefabSource = entry->Source;
        }

        AddSceneEditingChildren(app, imgui, editors, sources);
    }

    PrefabEditorPanel::PrefabEditorPanel(AssetId worldPrefab, string title, Application& app,
                                         AssetManager& assets, ImGuiLayer& imgui,
                                         TypeRegistry& types, EditorRegistry& /*editors*/,
                                         const AssetSourceIndex& /*sources*/, Input& input,
                                         InputRouter& router, SystemRegistry& systems,
                                         const PreviewLook* look)
        : m_Id(worldPrefab), m_Look(look), m_BaseTitle(std::move(title)),
          m_TitleId(fmt::format("##doc0x{:X}", worldPrefab.Value)), m_App(app), m_Assets(assets),
          m_Input(input), m_Router(router), m_Systems(systems)
    {
        m_Scene = Scene::Create(types);
        m_Context.Scene = m_Scene.get();
        m_Context.Assets = &assets;

        BuildScene();
    }

    string_view PrefabEditorPanel::GetTitle() const
    {
        // The title is invariant across edits: the "##" suffix is the window/dock identity and
        // the label before it never changes. The unsaved state is shown by the document window's
        // UnsavedDocument flag (an ImGui-drawn dot), not by mutating this string — an id change
        // here would drop keyboard focus from a field being edited in a docked child.
        m_DisplayTitle = fmt::format("{}{}", m_BaseTitle, m_TitleId);
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

        // The viewport opens under the preview look, when the document has one: its render block,
        // field of view and lighting.
        if (m_Look != nullptr)
        {
            if (m_Look->Render)
            {
                m_Viewport->ApplyRenderLook(*m_Look->Render);
            }
            m_Viewport->SetFovY(m_Look->FovY);
            m_Viewport->SetPreviewLook(*m_Look);
        }

        m_ViewportChild = AddChild(std::move(viewport));
        m_ExplorerChild = AddChild(std::move(explorer));
        m_InspectorChild = AddChild(std::move(inspector));
    }

    Veng::Renderer::Viewport* PrefabEditorPanel::GetDocumentViewport()
    {
        return m_Viewport != nullptr ? m_Viewport->GetViewport() : nullptr;
    }

    Scene* PrefabEditorPanel::GetDocumentScene()
    {
        if (!m_Context.IsPlaying())
        {
            return m_Context.Scene;
        }
        World* const world = m_App.GetWorldRunner().ResolveWorld(m_PlayWorld);
        return world != nullptr ? &world->GetScene() : nullptr;
    }

    PrefabEditorPanel::~PrefabEditorPanel()
    {
        // A document closed mid-Play stops its systems and gives the cursor back. Children (which
        // hold m_Context and m_Scene by reference) are released by the base after this body, and
        // the viewport child is still alive here for the presentation to unregister from.
        Stop();
        m_Scene.reset();
        m_Prefab = {};
    }

    void PrefabEditorPanel::Play()
    {
        if (m_Context.IsPlaying() || m_Scene == nullptr)
        {
            return;
        }

        // Run the systems over an independent clone so the authored scene is never mutated. Seed any
        // document-scoped state (a level seeds its settings entity) before the world starts, so the
        // spawn rules a system set runs at OnStart see the same initialized scene the runtime does.
        Unique<Scene> clone = m_Scene->Clone();
        SeedPlayScene(*clone);
        const Entity seat = ResolvePresentationSeat(*clone, Entity::Null);

        // A prefab document runs every registered system; a level document runs the ordered set
        // GetPlaySystems() names, so Play matches exactly what the level authored.
        vector<SystemId> systems;
        if (const vector<SystemId>* playSystems = GetPlaySystems(); playSystems != nullptr)
        {
            systems = *playSystems;
        }
        else
        {
            for (const SystemEntry& entry : m_Systems.Entries())
            {
                systems.push_back(entry.Id);
            }
        }

        WorldRunner& runner = m_App.GetWorldRunner();
        m_PlayWorld =
            runner.OpenWorld(WorldOpenInfo{.StartSimulation = true, .Systems = std::move(systems)},
                             std::move(clone));

        // The editor is not the game: the session may not travel, host or quit the editor, and its
        // own exit ends the session.
        m_App.SetWorldRequestPolicy(
            m_PlayWorld, WorldRequestPolicy{.Mode = WorldRequestMode::Sandboxed,
                                            .OnExit = [this](WorldInstanceId) { Stop(); }});

        // The document viewport presents the world through the clone's seat, pushing its own camera.
        m_PlayPresentation = GetDocumentViewport();
        if (m_PlayPresentation != nullptr)
        {
            m_App.GetManagedViewports().RegisterBoundViewport(
                *m_PlayPresentation,
                BoundViewportInfo{.World = m_PlayWorld, .Viewer = seat, .PullsCamera = false});
        }

        // The selection's handles index the edit scene, so drop it.
        m_Context.Clear();
        m_Context.Scene = &runner.ResolveWorld(m_PlayWorld)->GetScene();
        m_Context.Play = PlayState::Playing;
        m_Context.PlayAlpha = 0.0f;

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

        ReleaseFromPlay();
        m_PlayPause = WorldPauseScope{};
        if (m_PlayPresentation != nullptr)
        {
            m_App.GetManagedViewports().UnregisterBoundViewport(*m_PlayPresentation);
            m_PlayPresentation = nullptr;
        }

        // A world already closed (its own exit, a system, shutdown) has stopped and dropped
        // already, and closing it again is a no-op.
        const WorldInstanceId world = m_PlayWorld;
        m_PlayWorld = {};
        m_Context.Clear();
        m_Context.Scene = m_Scene.get();
        m_Context.Play = PlayState::Editing;
        m_Context.PlayAlpha = 0.0f;
        m_App.GetWorldRunner().CloseWorld(world);
    }

    void PrefabEditorPanel::Pause()
    {
        if (!m_Context.IsPlaying() || m_PlayPause.IsHeld())
        {
            return;
        }
        m_PlayPause = m_App.GetWorldRunner().PauseScope(m_PlayWorld);
        // A paused game is not consuming input; free the cursor for editor interaction.
        ReleaseFromPlay();
    }

    void PrefabEditorPanel::Resume()
    {
        if (!m_PlayPause.IsHeld())
        {
            return;
        }
        m_PlayPause = WorldPauseScope{};
        CaptureForPlay();
    }

    void PrefabEditorPanel::CaptureForPlay()
    {
        if (!m_Router.IsFocusTokenLive(m_Context.PlayCapture))
        {
            m_Context.PlayCapture = m_Router.PushFocus(InputFocus::Gameplay);
        }
        SyncPlayPointer();
    }

    void PrefabEditorPanel::ReleaseFromPlay()
    {
        if (m_Router.IsFocusTokenLive(m_Context.PlayCapture))
        {
            m_Router.PopFocus(m_Context.PlayCapture);
        }
        m_Context.PlayCapture = {};
        SyncPlayPointer();
    }

    void PrefabEditorPanel::SyncPlayPointer()
    {
        // Only while this document holds the capture: two documents can be playing at once, and the
        // router routes a captured pointer through the first viewport associated with the cursor seat.
        const bool holdsCapture =
            m_Context.IsPlaying() && m_Router.IsFocusTokenLive(m_Context.PlayCapture);
        const Renderer::Viewport* viewport = holdsCapture ? GetDocumentViewport() : nullptr;
        if (viewport != nullptr)
        {
            m_Router.AssociateViewportSeat(*viewport, m_Router.GetCursorSeat());
            m_PlayPointerViewport = viewport->GetId();
        }
        else if (m_PlayPointerViewport.IsValid())
        {
            m_Router.ClearViewportSeat(m_PlayPointerViewport);
            m_PlayPointerViewport = {};
        }
    }

    void PrefabEditorPanel::UpdatePlaySession()
    {
        if (!m_Context.IsPlaying())
        {
            return;
        }

        // The runner owns the session: a world that closed since last frame (its sandboxed exit, a
        // system closing it, shutdown) ends Play here.
        WorldRunner& runner = m_App.GetWorldRunner();
        World* const world = runner.ResolveWorld(m_PlayWorld);
        if (world == nullptr)
        {
            Stop();
            return;
        }
        m_Context.Scene = &world->GetScene();

        // Shift+Esc is this tool's own Play release, since a project's maps may bind no release of
        // their own. Read from the snapshot: a captured cursor starves the UI of key events.
        const bool shift = m_Input.IsKeyDown(Key::LeftShift) || m_Input.IsKeyDown(Key::RightShift);
        if (m_Router.IsGameplayFocused() && m_Router.IsFocusTokenLive(m_Context.PlayCapture) &&
            shift && m_Input.WasKeyPressed(Key::Escape))
        {
            ReleaseFromPlay();
        }

        // The capture also comes and goes outside this document's own calls: the viewport re-grabs
        // it on a click, and a ReleaseFocus role press pops it.
        SyncPlayPointer();

        m_Context.PlayAlpha = runner.ResolveAlpha(m_PlayWorld);
    }

    void PrefabEditorPanel::DrawDocumentToolbar()
    {
        const bool playing = m_Context.IsPlaying();

        // Save the document; greyed out with nothing unsaved. Mirrors File-menu Save / Ctrl+S,
        // dispatching to the document's own Save (a level saves its config too).
        {
            const UI::DisabledScope disabled = UI::Disabled(!HasUnsavedChanges());
            if (UI::IconButton(Icons::Save))
            {
                const VoidResult saved = Save();
                if (!saved)
                {
                    Log::Error("editor: save failed: {}", saved.error());
                }
            }
        }
        UI::Tooltip("Save the document (Ctrl+S)");

        UI::SameLine();
        UI::Separator();
        UI::SameLine();

        // Play transport: Play while editing; Stop + Pause/Resume while playing. Play draws
        // accent-filled while editing — a momentary click via an always-on toggle button, which
        // renders in the accent state and reports the click as its state flips off.
        {
            const UI::DisabledScope disabled = UI::Disabled(playing);
            bool playActive = true;
            if (UI::IconToggleButton(Icons::Play, playActive))
            {
                Play();
            }
        }
        UI::Tooltip("Clone the scene and run its systems (play in viewport)");

        UI::SameLine();
        {
            const UI::DisabledScope disabled = UI::Disabled(!playing);
            if (UI::IconButton(Icons::Stop))
            {
                Stop();
            }
            UI::Tooltip("Stop the play session and restore the edited scene");

            UI::SameLine();
            // The world's pause, so a pause the running game requested shows here too; only the
            // toolbar's own hold is the toolbar's to release.
            const bool paused = playing && m_App.IsWorldPaused(m_PlayWorld);
            const bool gamePaused = paused && !m_PlayPause.IsHeld();
            {
                const UI::DisabledScope heldByGame = UI::Disabled(gamePaused);
                if (UI::IconButton(paused ? Icons::Play : Icons::Pause))
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
            }
            UI::Tooltip(gamePaused ? "Paused by the running game"
                        : paused   ? "Resume the paused play session"
                                   : "Pause the play session (hold the current frame)");
        }

        UI::SameLine();
        UI::Separator();
        UI::SameLine();

        // Gizmo-mode group over the shared document mode (PrefabEditContext::Gizmo) every viewport
        // reads; mirrors the W/E/R keys. Disabled while playing (gizmos are an edit aid) — the keys
        // are gated the same way. The active mode's segment is accent-filled to read as selected.
        const UI::DisabledScope gizmoDisabled = UI::Disabled(playing);
        const UI::ButtonGroupItem gizmoModes[] = {
            {.Label = Icons::Translate, .Tooltip = "Translate (W)"},
            {.Label = Icons::Rotate, .Tooltip = "Rotate (E)"},
            {.Label = Icons::Scale, .Tooltip = "Scale (R)"},
        };
        i32 gizmoIndex = static_cast<i32>(m_Context.Gizmo);
        if (UI::ButtonGroup("##gizmo", gizmoIndex, gizmoModes))
        {
            m_Context.Gizmo = static_cast<GizmoMode>(gizmoIndex);
        }
    }

    void PrefabEditorPanel::OnUI()
    {
        UpdatePlaySession();

        if (m_Context.Scene == nullptr)
        {
            return;
        }

        if (const auto bar = UI::Toolbar("##prefab-toolbar"))
        {
            DrawDocumentToolbar();
            UI::SameLine();
            UI::Separator();
            UI::SameLine();
            usize editorOnly = 0;
            m_Context.Scene->Each<EditorOnly>([&editorOnly](Entity, EditorOnly&) { ++editorOnly; });
            UI::TextDisabled(
                fmt::format("{} entities", m_Context.Scene->EntityCount() - editorOnly));
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

        // Under a preview look the viewport lights the scene, with its controls; otherwise the
        // scene is only kept from rendering black.
        if (m_Look == nullptr)
        {
            AddPreviewLighting(*m_Scene);
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
