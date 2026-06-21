#include "panels/SceneViewportPanel.h"

#include <Veng/Asset/Mesh.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/ImGui/ImGuiTexture.h>
#include <Veng/Input.h>
#include <Veng/Math/AABB.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>
#include <Veng/Time.h>
#include <Veng/UI/UI.h>

#include "panels/PrefabEditorPanel.h"

#include <array>

namespace VengEditor
{
    using namespace Veng;

    SceneViewportPanel::SceneViewportPanel(Renderer::Context& context, AssetManager& assets,
                                           ImGuiLayer& imgui, PrefabEditContext& ctx, Input& input,
                                           PrefabEditorPanel& document)
        : m_Context(context), m_Assets(assets), m_ImGui(imgui), m_Ctx(ctx), m_Input(input),
          m_Document(document)
    {
        m_RenderExtent = context.GetInternalRenderExtent();

        m_SceneRenderer = Renderer::SceneRenderer::Create({
            .Context = context,
            .Assets = assets,
            .OutputFormat = context.GetOutputFormat(),
            .Extent = m_RenderExtent,
            .Settings = m_Settings,
        });

        // Seed a sensible opening view; the camera produces the live one each OnImGui.
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
        m_SceneTexture = imgui.CreateTexture(*m_SceneSampler, *m_SceneRenderer->GetOutput());
    }

    SceneViewportPanel::~SceneViewportPanel()
    {
        m_SceneTexture.reset();
        m_SceneSampler.reset();
        m_SceneRenderer.reset();
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

    void SceneViewportPanel::OnRender(Renderer::CommandBuffer& cmd)
    {
        if (m_Ctx.Scene == nullptr)
        {
            return;
        }

        // Resize and Configure both invalidate GetOutput(); rebind the ImGui texture
        // once after both are applied, before recording.
        bool outputInvalidated = false;

        if (m_PendingExtent.x != 0 && m_PendingExtent.y != 0 && m_PendingExtent != m_RenderExtent)
        {
            m_RenderExtent = m_PendingExtent;
            m_SceneRenderer->Resize(m_RenderExtent);
            outputInvalidated = true;
        }

        if (m_SettingsDirty)
        {
            m_SceneRenderer->Configure(m_Settings);
            m_SettingsDirty = false;
            outputInvalidated = true;
        }

        if (outputInvalidated)
        {
            m_SceneTexture = m_ImGui.CreateTexture(*m_SceneSampler, *m_SceneRenderer->GetOutput());
        }

        const Renderer::SceneView view{
            .World = *m_Ctx.Scene, .Camera = m_View, .Delta = Time::GetDeltaTime()};
        m_SceneRenderer->Execute(cmd, view);

        // ImGui's sampled read of the output is recorded outside the graph by
        // ImGuiLayer::Render, so transition the output to a sampleable layout here.
        cmd.PrepareForAccess(m_SceneRenderer->GetOutput(), Renderer::AccessKind::Sample);
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
            // The change is deferred via m_SettingsDirty so Configure runs in OnRender,
            // not mid-ImGui.
            static constexpr std::array<string_view, 10> modeNames{
                "Final",    "Albedo",    "Normal", "Depth",   "Roughness",
                "Metallic", "Occlusion", "AO",     "Shadows", "Cascades"};
            i32 mode = static_cast<i32>(m_Settings.Mode);
            UI::SetNextItemWidth(120.0f);
            if (UI::Combo("View", mode, modeNames))
            {
                m_Settings.Mode = static_cast<Renderer::DebugView>(mode);
                m_SettingsDirty = true;
            }
            UI::Tooltip("Debug visualization mode");

            UI::SameLine();
            if (UI::ToggleButton("Shadows", m_Settings.Shadows))
            {
                m_SettingsDirty = true;
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
        }
    }

    void SceneViewportPanel::OnImGui()
    {
        const vec2 available = UI::ContentRegionAvail();
        const uvec2 wanted{static_cast<u32>(available.x), static_cast<u32>(available.y)};
        m_PendingExtent = wanted;

        UI::Image(m_SceneTexture, available);

        // Camera gating: the image item drives hover; the window owns interaction focus.
        const bool hovered = UI::ItemHovered();
        const bool focused = UI::WindowFocused();

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
        in.Aspect = static_cast<f32>(m_RenderExtent.x) / static_cast<f32>(m_RenderExtent.y);

        const bool wantsCapture = m_Camera.Update(in, Time::GetDeltaTime());
        m_Input.SetMouseCaptured(wantsCapture);
        m_View = m_Camera.GetView();

        if (in.FrameSelection)
        {
            FrameSelection();
            m_View = m_Camera.GetView();
        }

        DrawToolbar();
    }
}
