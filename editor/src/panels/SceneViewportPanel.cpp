#include "panels/SceneViewportPanel.h"

#include "EditorIcons.h"

#include <Veng/Application.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Texture.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/ImGui/ImGuiTexture.h>
#include <Veng/Input.h>
#include <Veng/InputRouter.h>
#include <Veng/Math/AABB.h>
#include <Veng/Math/Ray.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DebugDraw.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneViewport.h>
#include <Veng/Scene/Transforms.h>
#include <Veng/Time.h>
#include <Veng/UI/UI.h>
#include <Veng/Vendor/ImGui.h>

#include "CommandStack.h"
#include "EditorCommand.h"

#include <array>

namespace VengEditor
{
    using namespace Veng;

    namespace
    {
        // The editor icon-pack texture ids (editor_icons.vengpack, mounted by EditorHost).
        constexpr AssetId LightIconId{0x9BA14A4E1E8AD8F4ULL};
        constexpr AssetId CameraIconId{0x010CD6BC54B24B5DULL};

        // Linear-RGBA gizmo colors.
        constexpr vec4 LightGizmoColor{1.0f, 0.85f, 0.4f, 1.0f};
        constexpr vec4 CameraGizmoColor{0.5f, 0.8f, 1.0f, 1.0f};
        // World-unit edge length of an icon billboard.
        constexpr f32 IconSize = 0.6f;
    }

    SceneViewportPanel::SceneViewportPanel(Application& app, AssetManager& assets,
                                           ImGuiLayer& imgui, PrefabEditContext& ctx, Input& input,
                                           InputRouter& router, CommandStack& commands)
        : m_Assets(assets), m_ImGui(imgui), m_Ctx(ctx), m_Input(input), m_Router(router),
          m_Commands(commands)
    {
        Renderer::Context& context = app.GetRenderContext();
        // A first-frame placeholder; the panel's content rect drives the real region each OnUI.
        const uvec2 extent = {1280, 720};

        m_Alive = CreateRef<bool>(true);

        // The editor draws light/camera gizmos through the engine debug-draw pass; enable it
        // from the start so the viewport's renderer wires the pass. Picking enables the id pass +
        // the billboard id-write so a viewport click resolves the entity under the cursor (mesh or
        // icon); enabled for the viewport's lifetime, not toggled per pick.
        m_Settings.DebugDraw = true;
        m_Settings.Picking = true;

        m_Viewport = Renderer::Viewport::Create({
            .Context = context,
            .Assets = assets,
            .Region = {.Offset = {0, 0}, .Extent = extent},
            .ColorFormat = context.GetOutputFormat(),
            .Settings = m_Settings,
            .Role = Renderer::ViewportRole::Offscreen,
            // Render only while this panel draws; an inactive dock tab pushes no ViewState, so the
            // engine skips it rather than rendering this scene behind the visible editor.
            .RenderOnDemand = true,
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

        // Resolve the gizmo icon textures from the editor icon pack (mounted by EditorHost).
        // A missing pack only drops the icon billboards; the wireframe gizmos still draw.
        if (const AssetResult<AssetHandle<Texture>> light = m_Assets.LoadSync<Texture>(LightIconId))
        {
            m_LightIcon = *light;
        }
        if (const AssetResult<AssetHandle<Texture>> camera =
                m_Assets.LoadSync<Texture>(CameraIconId))
        {
            m_CameraIcon = *camera;
        }
    }

    SceneViewportPanel::~SceneViewportPanel()
    {
        // Mark dead before dropping the viewport: a pick callback captures m_Alive by value, so any
        // resolve that would fire from a later Render is dropped rather than touching freed state.
        *m_Alive = false;
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

    void SceneViewportPanel::PushGizmos()
    {
        if (m_Ctx.Scene == nullptr)
        {
            return;
        }

        Renderer::DebugDraw& debug = m_Viewport->GetDebugDraw();

        // Lights: an icon billboard plus a wireframe of the falloff volume.
        for (auto [entity, transform, light] : m_Ctx.Scene->View<Transform, Light>())
        {
            const vec3 position = vec3(WorldMatrix(*m_Ctx.Scene, entity)[3]);

            if (m_LightIcon.IsLoaded())
            {
                // The pick id is the entity's packed slot index + 1 (0 = not pickable), the same
                // encoding the mesh id pass writes; clicking the icon selects this entity.
                debug.DrawBillboard(position, IconSize, m_LightIcon.Get()->GetHandle(),
                                    LightGizmoColor, entity.Index + 1u);
            }

            switch (light.Type)
            {
            case LightType::Point:
                debug.DrawSphere(position, light.Range, LightGizmoColor);
                break;
            case LightType::Spot:
            {
                // A spot cone: a ring at the falloff range plus four edges to the apex,
                // sized by the outer half-angle. The light's Direction is the cone axis.
                const vec3 axis = glm::length(light.Direction) > 0.0f
                                      ? glm::normalize(light.Direction)
                                      : vec3(0.0f, -1.0f, 0.0f);
                const f32 coneRadius = light.Range * std::tan(light.OuterCone);
                const vec3 center = position + axis * light.Range;

                // Build a basis around the axis for the cone ring.
                const vec3 up =
                    std::abs(axis.y) < 0.99f ? vec3(0.0f, 1.0f, 0.0f) : vec3(1.0f, 0.0f, 0.0f);
                const vec3 right = glm::normalize(glm::cross(axis, up));
                const vec3 bitangent = glm::cross(axis, right);

                constexpr u32 segments = 24;
                vec3 prev{};
                for (u32 i = 0; i <= segments; ++i)
                {
                    const f32 a =
                        glm::two_pi<f32>() * static_cast<f32>(i) / static_cast<f32>(segments);
                    const vec3 point =
                        center + coneRadius * (std::cos(a) * right + std::sin(a) * bitangent);
                    if (i > 0)
                    {
                        debug.DrawLine(prev, point, LightGizmoColor);
                    }
                    if (i % (segments / 4) == 0)
                    {
                        debug.DrawLine(position, point, LightGizmoColor);
                    }
                    prev = point;
                }
                break;
            }
            case LightType::Directional:
                // A directional light has no position-bound volume; a short arrow shows its axis.
                {
                    const vec3 axis = glm::length(light.Direction) > 0.0f
                                          ? glm::normalize(light.Direction)
                                          : vec3(0.0f, -1.0f, 0.0f);
                    debug.DrawLine(position, position + axis * 1.5f, LightGizmoColor);
                }
                break;
            }
        }

        // Cameras: an icon billboard plus the view frustum (clamped far so it stays readable).
        for (auto [entity, transform, camera] : m_Ctx.Scene->View<Transform, Camera>())
        {
            const mat4 world = WorldMatrix(*m_Ctx.Scene, entity);
            const vec3 position = vec3(world[3]);

            if (m_CameraIcon.IsLoaded())
            {
                debug.DrawBillboard(position, IconSize, m_CameraIcon.Get()->GetHandle(),
                                    CameraGizmoColor, entity.Index + 1u);
            }

            Camera gizmoCamera = camera;
            gizmoCamera.Far = glm::min(camera.Far, camera.Near + 5.0f);
            const CameraView view = MakeCameraView(gizmoCamera, 16.0f / 9.0f, world);
            debug.DrawFrustum(glm::inverse(view.ViewProjection()), CameraGizmoColor);
        }
    }

    void SceneViewportPanel::HandleClickToSelect(const bool hovered, const bool consumed)
    {
        // A pick is in flight (issued, not yet resolved): hold off so a held button does not queue a
        // burst. A camera drag / play-capture click is not a selection. Picking only resolves over an
        // edited scene, so skip while playing.
        if (m_PickInFlight || consumed || m_Ctx.Scene == nullptr || m_Ctx.IsPlaying())
        {
            return;
        }
        if (!hovered || !m_Input.WasMouseButtonPressed(MouseButton::Left))
        {
            return;
        }

        const bool additive =
            m_Input.IsKeyDown(Key::LeftControl) || m_Input.IsKeyDown(Key::RightControl) ||
            m_Input.IsKeyDown(Key::LeftSuper) || m_Input.IsKeyDown(Key::RightSuper);

        const vec2 mouse = m_Input.GetMousePosition();
        const ivec2 windowPoint{static_cast<i32>(mouse.x), static_cast<i32>(mouse.y)};

        m_PickInFlight = true;

        // The callback fires from a later Render (a frame or two on) on the render thread. It captures
        // the panel-alive flag by value: a resolve landing after the panel is torn down is dropped.
        // The renderer's own scene-epoch + caller-liveness guard drops a resolve after a Play/Stop
        // scene swap, so by the time this runs m_Ctx.Scene is the same scene the click was issued on.
        const Ref<bool> alive = m_Alive;
        m_Viewport->Pick(windowPoint,
                         [this, alive, additive](const optional<Entity> picked)
                         {
                             if (!*alive)
                             {
                                 return;
                             }
                             m_PickInFlight = false;

                             if (m_Ctx.Scene == nullptr)
                             {
                                 return;
                             }
                             if (!picked.has_value())
                             {
                                 // A background click clears the selection unless it is additive.
                                 if (!additive)
                                 {
                                     m_Ctx.Clear();
                                 }
                                 return;
                             }
                             if (additive)
                             {
                                 m_Ctx.Toggle(*picked);
                             }
                             else
                             {
                                 m_Ctx.SelectOnly(*picked);
                             }
                         });
    }

    bool SceneViewportPanel::HandleGizmo(const bool hovered, const bool consumed)
    {
        // No selection, a closed/playing scene, or a camera/play-capture click: the gizmo stands
        // down (a press falls through to click-to-select). An in-progress drag overrides hover/
        // consumed — once grabbed the gizmo keeps the mouse until release.
        if (m_Ctx.Scene == nullptr || m_Ctx.IsPlaying() || m_Ctx.Active.IsNull() ||
            !m_Ctx.Scene->IsAlive(m_Ctx.Active))
        {
            // The selection vanished mid-drag (deleted, scene swapped) — drop the drag, no commit.
            if (m_Gizmo.IsDragging() && m_Ctx.Scene != nullptr)
            {
                static_cast<void>(m_Gizmo.EndDrag(*m_Ctx.Scene, m_Ctx.Active));
            }
            return false;
        }

        const vec2 mouse = m_Input.GetMousePosition();
        const ivec2 windowPoint{static_cast<i32>(mouse.x), static_cast<i32>(mouse.y)};
        const optional<Ray> ray = m_Viewport->ScreenToWorldRay(windowPoint);

        // A drag in progress is serviced first, regardless of hover (the cursor may leave the arm).
        if (m_Gizmo.IsDragging())
        {
            if (m_Input.IsMouseButtonDown(MouseButton::Left) && ray.has_value())
            {
                m_Gizmo.Drag(*m_Ctx.Scene, m_Ctx.Active, *ray);
            }
            if (m_Input.WasMouseButtonReleased(MouseButton::Left) ||
                !m_Input.IsMouseButtonDown(MouseButton::Left))
            {
                if (const optional<std::pair<Transform, Transform>> edit =
                        m_Gizmo.EndDrag(*m_Ctx.Scene, m_Ctx.Active))
                {
                    OnCommit(edit->first, edit->second);
                }
            }
            return true;
        }

        if (consumed || !ray.has_value())
        {
            return false;
        }

        const vec3 cameraPosition = m_View.GetPosition();
        const bool overHandle =
            m_Gizmo.Hover(*m_Ctx.Scene, m_Ctx.Active, *ray, cameraPosition, m_Ctx.Gizmo);

        if (hovered && overHandle && m_Input.WasMouseButtonPressed(MouseButton::Left))
        {
            return m_Gizmo.BeginDrag(*m_Ctx.Scene, m_Ctx.Active, *ray, cameraPosition, m_Ctx.Gizmo);
        }

        // Hovering a handle (no press) still suppresses the click-select fall-through only on the
        // press frame; a bare hover does not consume, so report consumption only on a real grab.
        return false;
    }

    bool SceneViewportPanel::CursorOverGizmoHandle()
    {
        if (m_Ctx.IsPlaying() || m_Ctx.Scene == nullptr || m_Ctx.Active.IsNull() ||
            !m_Ctx.Scene->IsAlive(m_Ctx.Active))
        {
            return false;
        }

        const vec2 mouse = m_Input.GetMousePosition();
        const ivec2 windowPoint{static_cast<i32>(mouse.x), static_cast<i32>(mouse.y)};
        const optional<Ray> ray = m_Viewport->ScreenToWorldRay(windowPoint);
        if (!ray.has_value())
        {
            return false;
        }
        // m_View is last frame's here (the camera updates after this), exact enough for the press
        // hit-test; HandleGizmo re-tests below with this frame's view for the actual grab.
        return m_Gizmo.Hover(*m_Ctx.Scene, m_Ctx.Active, *ray, m_View.GetPosition(), m_Ctx.Gizmo);
    }

    void SceneViewportPanel::OnCommit(const Transform& start, const Transform& final)
    {
        // The drag applied the Transform live each frame; record it as one EditTransform spanning
        // the whole drag (start → final), so undo reverts the drag rather than a frame of it. The
        // command's Apply re-applies `final` (a no-op past the already-applied value), keeping the
        // edit durable at the commit boundary.
        if (m_Ctx.Scene == nullptr || m_Ctx.Active.IsNull() ||
            !m_Ctx.Scene->IsAlive(m_Ctx.Active) ||
            m_Ctx.Scene->TryGet<Transform>(m_Ctx.Active) == nullptr)
        {
            return;
        }
        // A drag that ended exactly where it began is no edit at all.
        if (start.Position == final.Position && start.Rotation == final.Rotation &&
            start.Scale == final.Scale)
        {
            return;
        }
        m_Commands.Push(CreateUnique<EditTransform>(m_Ctx.Active, start, final));
    }

    void SceneViewportPanel::ApplyLevelRenderSettings(const LevelRenderSettings& render)
    {
        // Run the level's post/pipeline subset through the shared runtime mapping so the
        // level→renderer wiring lives in one place; the sky/environment is resolved from the scene's
        // components each frame (ApplySceneSky in OnUI). Start the topology half from the live
        // settings so the editor-only bits (DebugDraw, the debug-view Mode, the toolbar toggles)
        // survive, and seed the per-frame half into a scratch view we pull the level-owned values from.
        Renderer::SceneRendererSettings next = m_Settings;
        Renderer::ViewState scratch;
        Veng::ApplyLevelRenderSettings(render, next, scratch);

        // A topology change is the only thing that needs a Configure recompile, and this is called
        // per settings-panel edit (an Exposure drag too), so flip dirty only when a toggle moved.
        if (next.Bloom != m_Settings.Bloom || next.Shadows != m_Settings.Shadows ||
            next.AO != m_Settings.AO)
        {
            m_Settings = next;
            m_SettingsDirty = true;
        }

        m_Exposure = scratch.Exposure;
        m_BloomIntensity = scratch.BloomIntensity;
    }

    void SceneViewportPanel::DrawToolbar()
    {
        if (auto bar = UI::ViewportOverlay("##viewport-toolbar", UI::OverlayAnchor::TopLeft))
        {
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
            if (UI::Button(Icons::Frame))
            {
                FrameSelection();
            }
            UI::Tooltip("Frame the selection, or the whole scene (F)");

            UI::Separator();
            UI::SameLine();

            // Debug visualizations: the DebugView dropdown plus the battery toggles.
            // The change is deferred via m_SettingsDirty so Configure runs once in OnUI,
            // not per widget.
            i32 mode = static_cast<i32>(m_Settings.Mode);
            UI::SetNextItemWidth(120.0f);
            if (UI::Combo("View", mode, Renderer::DebugViewNames))
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
        // A click the camera (or play capture) consumed is navigation, not a selection: an RMB-fly,
        // an MMB-pan, or an Alt-orbit drag. Tracked so HandleClickToSelect only picks on a bare click.
        bool cameraConsumed = gameplayFocused;
        if (gameplayFocused)
        {
            m_View = m_Camera.GetView();
        }
        else
        {
            // A camera mouse-drag (LMB dolly, Alt-orbit, MMB pan, RMB fly) may only begin with a
            // press over the viewport image that is not grabbing a gizmo handle, and then owns the
            // drag until every mouse button releases. This keeps a drag that wanders onto a dock tab
            // or the toolbar — and a press on a gizmo handle (which HandleGizmo claims below) — from
            // moving the camera. The gizmo only grabs on a left press, so a right/middle press always
            // owns navigation.
            const bool anyMouseDown = in.MouseLeft || in.MouseRight || in.MouseMiddle;
            if (!anyMouseDown || m_Gizmo.IsDragging())
            {
                m_CameraDragOwned = false;
            }
            else if (!m_CameraDragOwned && hovered)
            {
                const bool navPress =
                    m_Input.WasMouseButtonPressed(MouseButton::Right) ||
                    m_Input.WasMouseButtonPressed(MouseButton::Middle) ||
                    (m_Input.WasMouseButtonPressed(MouseButton::Left) && !CursorOverGizmoHandle());
                if (navPress)
                {
                    m_CameraDragOwned = true;
                }
            }
            if (!m_CameraDragOwned)
            {
                in.MouseLeft = false;
                in.MouseRight = false;
                in.MouseMiddle = false;
            }

            const bool navCursorLock = m_Camera.Update(in, Time::GetDeltaTime());
            m_Input.SetMouseCaptured(navCursorLock);
            m_View = m_Camera.GetView();
            cameraConsumed = navCursorLock || in.MouseRight || in.MouseMiddle || in.Alt;

            if (in.FrameSelection)
            {
                FrameSelection();
                m_View = m_Camera.GetView();
            }
        }

        // Push this frame's render source onto the viewport: a null Scene (a closed document)
        // is a no-op in Render. Play always renders through the scene's authored Viewer camera
        // (what the player sees); edit mode, Stop, and a scene that authors no camera use the
        // editor camera.
        CameraView camera = m_View;
        if (m_Ctx.IsPlaying() && m_Ctx.Scene != nullptr)
        {
            const f32 aspect = static_cast<f32>(renderExtent.x) / static_cast<f32>(renderExtent.y);
            if (const optional<CameraView> resolved =
                    ResolvePrimaryCameraView(*m_Ctx.Scene, aspect))
            {
                camera = *resolved;
            }
        }

        Renderer::ViewState view{
            .World = m_Ctx.Scene,
            .Camera = camera,
            .Delta = Time::GetDeltaTime(),
            .Exposure = m_Exposure,
            .BloomIntensity = m_BloomIntensity,
        };

        // Resolve the scene's sky/lighting components (Environment / Atmosphere / Skylight) and its
        // directional sun onto this frame's view and the sky topology — the same mapping the runtime
        // uses, so adding a component in the editor lights the scene immediately. A sky topology
        // change flags a Configure, applied next frame at the top of OnUI (one frame of latency, like
        // the rest of the editor's settings edits).
        if (m_Ctx.Scene != nullptr)
        {
            Renderer::SceneRendererSettings next = m_Settings;
            Veng::ApplySceneSky(*m_Ctx.Scene, next, view);
            if (next.Skybox != m_Settings.Skybox || next.Atmosphere != m_Settings.Atmosphere ||
                next.Skylight != m_Settings.Skylight)
            {
                m_Settings = next;
                m_SettingsDirty = true;
            }
        }

        m_Viewport->SetViewState(view);

        // Mode keys select the gizmo mode, but only while not flying: the RMB fly-camera binds
        // W/A/S/D/E/Q, so reading W/E/R as gizmo keys mid-fly would fight movement. ScreenToWorldRay
        // needs the camera from the SetViewState above, so this runs after it.
        if (!m_Ctx.IsPlaying() && !cameraConsumed && (hovered || focused))
        {
            if (m_Input.WasKeyPressed(Key::W))
            {
                m_Ctx.Gizmo = GizmoMode::Translate;
            }
            else if (m_Input.WasKeyPressed(Key::E))
            {
                m_Ctx.Gizmo = GizmoMode::Rotate;
            }
            else if (m_Input.WasKeyPressed(Key::R))
            {
                m_Ctx.Gizmo = GizmoMode::Scale;
            }
        }

        // Gizmo-first on the shared content-rect mouse path: a press over a handle (or an in-flight
        // drag) is a manipulation and suppresses the click-to-select fall-through; a press not over
        // a handle leaves the gizmo idle and falls through to selection below.
        const bool gizmoConsumed = HandleGizmo(hovered, cameraConsumed);

        // A bare left-click inside the content rect issues an id-buffer pick → selection. After
        // SetViewState so the renderer's pick guard captures the scene this frame renders.
        HandleClickToSelect(hovered, cameraConsumed || gizmoConsumed);

        // Push the light/camera gizmos for the next render. The engine renders the viewport at the
        // top of the next frame, so the accumulator filled here is consumed then (one frame of
        // latency, the same as the pushed ViewState). Skipped while playing — gizmos are an edit aid.
        if (!m_Ctx.IsPlaying())
        {
            PushGizmos();

            // The manipulation gizmo on the active entity, into the same per-viewport channel.
            if (m_Ctx.Scene != nullptr && !m_Ctx.Active.IsNull() &&
                m_Ctx.Scene->IsAlive(m_Ctx.Active))
            {
                m_Gizmo.Draw(m_Viewport->GetDebugDraw(), *m_Ctx.Scene, m_Ctx.Active,
                             m_View.GetPosition(), m_Ctx.Gizmo);
            }
        }

        DrawToolbar();
        if (gameplayFocused)
        {
            DrawCaptureNotice();
        }
    }
}
