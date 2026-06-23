#include "panels/SceneViewportPanel.h"

#include <Veng/Application.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/ImGui/ImGuiTexture.h>
#include <Veng/Input.h>
#include <Veng/InputRouter.h>
#include <Veng/Math/AABB.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>
#include <Veng/Time.h>
#include <Veng/UI/UI.h>
#include <Veng/Vendor/ImGui.h>

#include "panels/PrefabEditorPanel.h"

#include <array>

namespace VengEditor
{
    using namespace Veng;

    SceneViewportPanel::SceneViewportPanel(Application& app, AssetManager& assets,
                                           ImGuiLayer& imgui, PrefabEditContext& ctx, Input& input,
                                           InputRouter& router, PrefabEditorPanel& document)
        : m_Assets(assets), m_ImGui(imgui), m_Ctx(ctx), m_Input(input), m_Router(router),
          m_Document(document)
    {
        Renderer::Context& context = app.GetRenderContext();
        const uvec2 extent = context.GetInternalRenderExtent();

        m_Viewport = Renderer::Viewport::Create({
            .Context = context,
            .Assets = assets,
            .Region = {.Offset = {0, 0}, .Extent = extent},
            .ColorFormat = context.GetOutputFormat(),
            .Settings = m_Settings,
            .Role = Renderer::ViewportRole::Offscreen,
        });
        app.RegisterViewport(*m_Viewport);
        m_TextureExtent = extent;

        // Seed a sensible opening view; the camera produces the live one each OnUI.
        m_View = m_Camera.GetView();

        // Edge clamping prevents sampling past the image boundary when the panel size
        // does not align to a texel.
        m_SceneSampler = Renderer::Sampler::Create(
            context, {
                         .Name = "Scene Viewport Sampler",
                         .AddressModeU = Renderer::AddressMode::ClampToEdge,
                         .AddressModeV = Renderer::AddressMode::ClampToEdge,
                         .AddressModeW = Renderer::AddressMode::ClampToEdge,
                     });
        m_SceneTexture = imgui.CreateTexture(*m_SceneSampler, *m_Viewport->GetOutput());
    }

    SceneViewportPanel::~SceneViewportPanel()
    {
        m_SceneTexture.reset();
        m_SceneSampler.reset();
        m_Viewport.reset();
    }

    void SceneViewportPanel::FrameSelection()
    {
        if (m_Ctx.Scene == nullptr)
        {
            return;
        }

        // Union the selected entities' world bounds; fall back to the whole scene when
        // nothing is selected or no selected entity carries a resident mesh.
        AABB bounds = AABB::Empty();
        for (const Entity entity : m_Ctx.Selection)
        {
            if (!m_Ctx.Scene->IsAlive(entity))
            {
                continue;
            }
            const MeshRenderer* renderer = m_Ctx.Scene->TryGet<MeshRenderer>(entity);
            if (renderer == nullptr || !renderer->Mesh.IsLoaded())
            {
                continue;
            }
            const mat4 world = WorldMatrix(*m_Ctx.Scene, entity);
            bounds.Expand(renderer->Mesh.Get()->GetBounds().Transformed(world));
        }

        if (bounds.IsEmpty())
        {
            bounds = SceneBounds(*m_Ctx.Scene);
        }
        if (bounds.IsEmpty())
        {
            return;
        }

        const vec3 center = bounds.Center();
        const f32 radius = glm::max(glm::length(bounds.Extents()), 0.1f);
        m_Camera.Frame(center, radius);
    }

    void SceneViewportPanel::ApplyLevelRenderSettings(const LevelRenderSettings& render)
    {
        if (m_Settings.Bloom != render.Bloom || m_Settings.Shadows != render.Shadows ||
            m_Settings.AO != render.AO)
        {
            m_Settings.Bloom = render.Bloom;
            m_Settings.Shadows = render.Shadows;
            m_Settings.AO = render.AO;
            m_SettingsDirty = true;
        }
        m_Exposure = render.Exposure;
        m_BloomIntensity = render.BloomIntensity;
    }

    void SceneViewportPanel::DrawToolbar()
    {
        const UI::Theme& theme = UI::GetTheme();
        const bool playing = m_Ctx.IsPlaying();

        if (auto bar = UI::ViewportOverlay("##viewport-toolbar", UI::OverlayAnchor::TopLeft))
        {
            // Gameplay preview: Play while editing; Stop + Pause/Resume while playing.
            {
                const UI::DisabledScope disabled = UI::Disabled(playing);
                const UI::StyleColorScope accent =
                    UI::StyleColor(UI::StyleColorId::Button, theme.Accent);
                if (UI::Button("Play"))
                {
                    m_Document.Play();
                }
            }
            UI::Tooltip("Clone the scene and run its systems (play in viewport)");

            UI::SameLine();
            {
                const UI::DisabledScope disabled = UI::Disabled(!playing);
                if (UI::Button("Stop"))
                {
                    m_Document.Stop();
                }
                UI::Tooltip("Stop the play session and restore the edited scene");

                UI::SameLine();
                const bool paused = m_Ctx.Play == PlayState::Paused;
                if (UI::Button(paused ? "Resume" : "Pause"))
                {
                    if (paused)
                    {
                        m_Document.Resume();
                    }
                    else
                    {
                        m_Document.Pause();
                    }
                }
                UI::Tooltip(paused ? "Resume the paused play session"
                                   : "Pause the play session (hold the current frame)");

                UI::SameLine();
                (void)UI::ToggleButton("Game view", m_ViewThroughScene);
                UI::Tooltip("Render through the scene's Viewer camera (what the player sees)");
            }

            UI::Separator();
            UI::SameLine();

            // Camera: fly speed, FOV, and a frame-selection shortcut.
            f32 flySpeed = m_Camera.GetFlySpeed();
            UI::SetNextItemWidth(110.0f);
            if (UI::Drag("Speed", flySpeed,
                         {.Speed = 0.1f, .Min = 0.1f, .Max = 200.0f, .Format = "%.1f m/s"}))
            {
                m_Camera.SetFlySpeed(flySpeed);
            }
            UI::Tooltip("Fly-camera movement speed");

            UI::SameLine();
            f32 fovDegrees = glm::degrees(m_Camera.GetFovY());
            UI::SetNextItemWidth(110.0f);
            if (UI::Slider("FOV", fovDegrees, {.Min = 20.0f, .Max = 110.0f, .Format = "%.0f deg"}))
            {
                m_Camera.SetFovY(glm::radians(fovDegrees));
            }
            UI::Tooltip("Vertical field of view");

            UI::SameLine();
            if (UI::Button("Frame"))
            {
                FrameSelection();
            }
            UI::Tooltip("Frame the selection, or the whole scene (F)");

            UI::Separator();
            UI::SameLine();

            // Debug visualizations: the DebugView dropdown plus the battery toggles.
            // The change is deferred via m_SettingsDirty so Configure runs once in OnUI,
            // not per widget.
            // Entries mirror the DebugView enum in declaration order; combo index == enum value.
            static constexpr std::array<string_view, 13> modeNames{
                "Final",         "Albedo", "Normal",  "Depth",    "Roughness",        "Metallic",
                "Occlusion",     "AO",     "Shadows", "Cascades", "Punctual shadows", "Bloom",
                "Motion vectors"};
            i32 mode = static_cast<i32>(m_Settings.Mode);
            UI::SetNextItemWidth(120.0f);
            if (UI::Combo("View", mode, modeNames))
            {
                m_Settings.Mode = static_cast<Renderer::DebugView>(mode);
                m_SettingsDirty = true;
            }
            UI::Tooltip("Debug visualization mode");

            UI::SameLine();
            // Shadows are two independent arms behind one combo: Settings.Shadows is the
            // directional cascade, Settings.PunctualShadows the point/spot atlas. A scene's
            // visible shadows can come from either, so both are exposed; each drives a
            // Configure recompile.
            UI::SetNextItemWidth(120.0f);
            if (auto shadowMenu = UI::ComboBox("##shadows", "Shadows"))
            {
                if (UI::Checkbox("Directional", m_Settings.Shadows))
                {
                    m_SettingsDirty = true;
                }
                if (UI::Checkbox("Punctual", m_Settings.PunctualShadows))
                {
                    m_SettingsDirty = true;
                }
            }
            UI::SameLine();
            if (UI::ToggleButton("AO", m_Settings.AO))
            {
                m_SettingsDirty = true;
            }
            UI::SameLine();
            if (UI::ToggleButton("Bloom", m_Settings.Bloom))
            {
                m_SettingsDirty = true;
            }
            UI::SameLine();
            if (UI::ToggleButton("TAA", m_Settings.TAA))
            {
                m_SettingsDirty = true;
            }
            UI::Tooltip("Temporal anti-aliasing");
        }
    }

    void SceneViewportPanel::DrawCaptureNotice()
    {
        const UI::Theme& theme = UI::GetTheme();
        if (auto banner = UI::ViewportOverlay("##capture-notice", UI::OverlayAnchor::TopCenter))
        {
            UI::TextColored(theme.Accent, "Mouse captured  —  Shift+Esc to release");
        }
    }

    void SceneViewportPanel::OnUI()
    {
        // The engine renders the viewport at frame start, applying any pending region resize
        // and Configure before this runs; the output the panel samples is the one its prior
        // SetRegion/SetViewState produced. Re-fetch the ImGui texture when that applied extent
        // differs from the one the current texture views (a resize or Configure invalidated it).
        const uvec2 appliedExtent = m_Viewport->GetRegion().Extent;
        if (appliedExtent != m_TextureExtent && appliedExtent.x != 0 && appliedExtent.y != 0)
        {
            m_SceneTexture = m_ImGui.CreateTexture(*m_SceneSampler, *m_Viewport->GetOutput());
            m_TextureExtent = appliedExtent;
        }

        const vec2 available = UI::ContentRegionAvail();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const uvec2 wanted{static_cast<u32>(available.x), static_cast<u32>(available.y)};

        // Feed the content rect to the viewport: the extent drives the debounced resize, the
        // offset is the panel's window-space origin (the picking seam maps from it). A zero
        // extent (a collapsed or first-frame panel) is ignored by SetRegion.
        m_Viewport->SetRegion({
            .Offset = {static_cast<i32>(origin.x), static_cast<i32>(origin.y)},
            .Extent = wanted,
        });

        // Settings edits (debug view, battery toggles) reconfigure the renderer; this
        // invalidates the output, so re-fetch the texture immediately.
        if (m_SettingsDirty)
        {
            m_Viewport->Configure(m_Settings);
            m_SettingsDirty = false;
            m_SceneTexture = m_ImGui.CreateTexture(*m_SceneSampler, *m_Viewport->GetOutput());
        }

        UI::Image(m_SceneTexture, available);

        // Camera gating: the image item drives hover; the window owns interaction focus.
        const bool hovered = UI::ItemHovered();
        const bool focused = UI::WindowFocused();

        // Play mouse capture is the router's gameplay focus (pushed by the document on Play,
        // popped by Shift+Esc or window-focus loss). Clicking the viewport while playing
        // re-grabs it after a release; recompute focus after so the push takes effect this frame.
        if (m_Ctx.IsPlaying() && !m_Router.IsGameplayFocused() && hovered &&
            m_Input.WasMouseButtonPressed(MouseButton::Left))
        {
            m_Router.PushFocus(InputFocus::Gameplay);
        }
        const bool gameplayFocused = m_Router.IsGameplayFocused();

        if (gameplayFocused)
        {
            UI::ItemBorder(UI::GetTheme().Accent, 3.0f);
        }

        // The render extent the camera's aspect is computed against is the viewport's, which
        // tracks the panel; fall back to the content rect on a degenerate first frame.
        const uvec2 renderExtent = appliedExtent.x != 0 && appliedExtent.y != 0 ? appliedExtent
                                   : wanted.x != 0 && wanted.y != 0             ? wanted
                                                                                : uvec2{1, 1};

        EditorCameraInput in;
        in.Hovered = hovered;
        in.Focused = focused;
        in.MouseDelta = m_Input.GetMouseDelta();
        in.ScrollDelta = m_Input.GetScrollDelta();
        in.MouseLeft = m_Input.IsMouseButtonDown(MouseButton::Left);
        in.MouseRight = m_Input.IsMouseButtonDown(MouseButton::Right);
        in.MouseMiddle = m_Input.IsMouseButtonDown(MouseButton::Middle);
        in.Alt = m_Input.IsKeyDown(Key::LeftAlt) || m_Input.IsKeyDown(Key::RightAlt);
        in.Shift = m_Input.IsKeyDown(Key::LeftShift) || m_Input.IsKeyDown(Key::RightShift);
        in.Forward = m_Input.IsKeyDown(Key::W);
        in.Back = m_Input.IsKeyDown(Key::S);
        in.Left = m_Input.IsKeyDown(Key::A);
        in.Right = m_Input.IsKeyDown(Key::D);
        in.Up = m_Input.IsKeyDown(Key::E);
        in.Down = m_Input.IsKeyDown(Key::Q);
        in.FrameSelection = (hovered || focused) && m_Input.WasKeyPressed(Key::F);
        in.Aspect = static_cast<f32>(renderExtent.x) / static_cast<f32>(renderExtent.y);

        // While the game owns input the editor camera stands down (the router already holds
        // the cursor captured); otherwise the camera reads input and drives its own transient
        // navigation cursor lock for the RMB-fly drag.
        if (gameplayFocused)
        {
            m_View = m_Camera.GetView();
        }
        else
        {
            const bool navCursorLock = m_Camera.Update(in, Time::GetDeltaTime());
            m_Input.SetMouseCaptured(navCursorLock);
            m_View = m_Camera.GetView();

            if (in.FrameSelection)
            {
                FrameSelection();
                m_View = m_Camera.GetView();
            }
        }

        // Push this frame's render source onto the viewport: a null Scene (a closed document)
        // is a no-op in Render. Play with the toggle on previews the scene's authored Viewer
        // camera (what the player sees); edit mode, Stop, and an unauthored scene use the
        // editor camera.
        CameraView camera = m_View;
        if (m_Ctx.IsPlaying() && m_ViewThroughScene && m_Ctx.Scene != nullptr)
        {
            const f32 aspect = static_cast<f32>(renderExtent.x) / static_cast<f32>(renderExtent.y);
            if (const optional<CameraView> resolved =
                    ResolvePrimaryCameraView(*m_Ctx.Scene, aspect))
            {
                camera = *resolved;
            }
        }

        m_Viewport->SetViewState({
            .World = m_Ctx.Scene,
            .Camera = camera,
            .Delta = Time::GetDeltaTime(),
            .Exposure = m_Exposure,
            .BloomIntensity = m_BloomIntensity,
        });

        DrawToolbar();
        if (gameplayFocused)
        {
            DrawCaptureNotice();
        }
    }
}
