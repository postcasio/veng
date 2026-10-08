#include "panels/SceneViewportPanel.h"

#include "EditorIcons.h"

#include <Veng/Renderer/SceneGizmos.h>

#include <Veng/Application.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Environment.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Texture.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/ImGui/ImGuiTexture.h>
#include <Veng/Input.h>
#include <Veng/InputRouter.h>
#include <Veng/Log.h>
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
#include "EditorOnly.h"

#include <array>
#include <cmath>

namespace VengEditor
{
    using namespace Veng;

    namespace
    {
        // The editor icon-pack texture ids (editor_icons.vengpack, mounted by EditorHost).
        constexpr AssetId LightIconId{0x9BA14A4E1E8AD8F4ULL};
        constexpr AssetId CameraIconId{0x010CD6BC54B24B5DULL};

        // World-unit edge length of an icon billboard.
        constexpr f32 IconSize = 0.6f;

        // SceneLighting::SunIntensity is in these, so its slider reads in a friendly range.
        constexpr f32 LuxPerSunUnit = 10000.0f;

        constexpr const char* SettingsPopup = "ViewportSettings";

        // The unit vector toward a light at `yaw` about +Y and `pitch` above the horizon.
        vec3 SunTowards(f32 yaw, f32 pitch)
        {
            const f32 c = std::cos(pitch);
            return {c * std::cos(yaw), std::sin(pitch), c * std::sin(yaw)};
        }
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
        m_DefaultSettings = m_Settings;
        m_DefaultFovY = m_Camera.GetFovY();

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

        // The whole layer is the engine's, so an editor viewport and a game's debug view show the
        // same stand-ins for the same content. The editor's part is the art it ships and the
        // picking: a billboard carrying its entity's pick id is what makes a light selectable by
        // clicking its icon, which a game drawing the layer to look at has no use for.
        const Renderer::SceneGizmoStyle style{
            .LightIcon =
                m_LightIcon.IsLoaded() ? m_LightIcon.Get()->GetHandle() : Renderer::TextureHandle{},
            .CameraIcon = m_CameraIcon.IsLoaded() ? m_CameraIcon.Get()->GetHandle()
                                                  : Renderer::TextureHandle{},
            .IconSize = IconSize,
            .Pickable = true,
        };
        Renderer::DrawSceneGizmos(*m_Ctx.Scene, m_Viewport->GetDebugDraw(),
                                  Renderer::SceneGizmo::Lights | Renderer::SceneGizmo::Cameras,
                                  style);
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

    void SceneViewportPanel::SetFovY(f32 fovY)
    {
        m_Camera.SetFovY(fovY);
        m_DefaultFovY = fovY;
    }

    void SceneViewportPanel::SetPreviewLook(const PreviewLook& look)
    {
        m_Look = &look;
        m_LightingReady = false;
    }

    void SceneViewportPanel::ApplyLevelRenderSettings(const LevelRenderSettings& render)
    {
        // Run the render block through the shared runtime mapping so the level→renderer wiring
        // lives in one place; the sky is the scene's Sky component, resolved by the renderer itself.
        // The editor-only bits (DebugDraw, Picking, the debug view) survive from the live settings.
        m_Render = render;
        Renderer::SceneRendererSettings next = m_Settings;
        Veng::ApplyLevelRenderSettings(render, next, m_BaseView);

        // Called per settings-panel edit (an Exposure drag too), so reconfigure only when the
        // topology actually moved.
        if (next != m_Settings)
        {
            m_Settings = next;
            m_SettingsDirty = true;
        }
    }

    void SceneViewportPanel::ResetAll()
    {
        Renderer::SceneRendererSettings next = m_DefaultSettings;
        m_BaseView = {};
        if (m_Render)
        {
            Veng::ApplyLevelRenderSettings(*m_Render, next, m_BaseView);
        }
        if (next != m_Settings)
        {
            m_Settings = next;
            m_SettingsDirty = true;
        }
        m_Camera.SetFovY(m_DefaultFovY);
        m_Lighting = m_DefaultLighting;
    }

    void SceneViewportPanel::ApplyPreviewLighting()
    {
        if (m_Look == nullptr || m_Ctx.Scene == nullptr || m_Ctx.IsPlaying())
        {
            return;
        }
        Scene& scene = *m_Ctx.Scene;

        // A sky the document authors takes precedence; the editor's stands aside while there is one.
        m_OwnSky = false;
        scene.Each<Sky>([&](Entity entity, Sky&)
                        { m_OwnSky = m_OwnSky || !scene.Has<EditorOnly>(entity); });

        if (m_Lighting.Environment != m_LoadedEnvironment || !m_LightingReady)
        {
            const AssetId wanted = m_LightingReady ? m_Lighting.Environment : m_Look->Environment;
            m_LoadedEnvironment = wanted;
            m_Environment = {};
            if (wanted.IsValid())
            {
                if (auto loaded = m_Assets.LoadSync<EnvironmentMap>(wanted))
                {
                    m_Environment = *loaded;
                }
                else
                {
                    Log::Warn("Scene viewport: environment 0x{:X} did not load: {}", wanted.Value,
                              loaded.error().Detail);
                }
            }
            if (!m_LightingReady)
            {
                // The defaults read the scene before the editor adds anything to it.
                m_DefaultLighting = DefaultSceneLighting(
                    scene, m_Environment.IsValid() ? m_Look->Environment : AssetId{});
                m_Lighting = m_DefaultLighting;
                m_LoadedEnvironment = m_Lighting.Environment;
                m_LightingReady = true;
            }
        }

        const bool wantSky = !m_OwnSky && m_Environment.IsValid();
        if (wantSky && (m_PreviewSky.IsNull() || !scene.IsAlive(m_PreviewSky)))
        {
            m_PreviewSky = scene.CreateEntity();
            scene.Add<EditorOnly>(m_PreviewSky);
            scene.Add<Name>(m_PreviewSky) = Name{.Value = "Preview Sky"};
            scene.Add<Sky>(m_PreviewSky).Lighting = SkyLighting::IBL;
        }
        else if (!wantSky && !m_PreviewSky.IsNull() && scene.IsAlive(m_PreviewSky))
        {
            scene.DestroyEntity(m_PreviewSky);
            m_PreviewSky = {};
        }
        if (wantSky)
        {
            auto& sky = scene.Get<Sky>(m_PreviewSky);
            sky.Intensity = m_Lighting.EnvIntensity;
            auto* source =
                static_cast<EnvironmentSky*>(sky.Source.SetActive(TypeIdOf<EnvironmentSky>()));
            source->Map = m_Environment;
        }

        if (m_Lighting.Sun && (m_PreviewSun.IsNull() || !scene.IsAlive(m_PreviewSun)))
        {
            m_PreviewSun = scene.CreateEntity();
            scene.Add<EditorOnly>(m_PreviewSun);
            scene.Add<Name>(m_PreviewSun) = Name{.Value = "Preview Light"};
            scene.Add<Light>(m_PreviewSun) = Light{.Type = LightType::Directional};
        }
        else if (!m_Lighting.Sun && !m_PreviewSun.IsNull() && scene.IsAlive(m_PreviewSun))
        {
            scene.DestroyEntity(m_PreviewSun);
            m_PreviewSun = {};
        }
        if (m_Lighting.Sun)
        {
            auto& sun = scene.Get<Light>(m_PreviewSun);
            sun.Direction = -SunTowards(m_Lighting.SunYaw, m_Lighting.SunPitch);
            sun.Color = m_Lighting.SunColor;
            sun.Intensity = m_Lighting.SunIntensity * LuxPerSunUnit;
        }
    }

    void SceneViewportPanel::DrawToolbar()
    {
        if (const auto bar = UI::ViewportOverlay("##viewport-toolbar", UI::OverlayAnchor::TopLeft))
        {
            f32 flySpeed = m_Camera.GetFlySpeed();
            UI::SetNextItemWidth(110.0f);
            if (UI::Drag("Speed", flySpeed,
                         {.Speed = 0.1f, .Min = 0.1f, .Max = 200.0f, .Format = "%.1f m/s"}))
            {
                m_Camera.SetFlySpeed(flySpeed);
            }
            UI::Tooltip("Fly-camera movement speed");

            UI::SameLine();
            if (UI::IconButton(Icons::Frame))
            {
                FrameSelection();
            }
            UI::Tooltip("Frame the selection, or the whole scene (F)");

            UI::Separator();
            UI::SameLine();

            // A debug view is a Configure recompile, deferred via m_SettingsDirty to OnUI.
            i32 mode = static_cast<i32>(m_Settings.Mode);
            UI::SetNextItemWidth(120.0f);
            if (UI::Combo("##view", mode, Renderer::DebugViewNames))
            {
                m_Settings.Mode = static_cast<Renderer::DebugView>(mode);
                m_SettingsDirty = true;
            }
            UI::Tooltip("Debug visualization mode");

            if (m_Look != nullptr && !m_OwnSky)
            {
                UI::SameLine();
                vector<string_view> names{"No Environment"};
                i32 env = 0;
                for (usize i = 0; i < m_Look->Environments.size(); ++i)
                {
                    names.push_back(m_Look->Environments[i].first);
                    if (m_Look->Environments[i].second == m_Lighting.Environment)
                    {
                        env = static_cast<i32>(i + 1);
                    }
                }
                UI::SetNextItemWidth(120.0f);
                if (UI::Combo("##environment", env, names))
                {
                    m_Lighting.Environment =
                        env == 0 ? AssetId{}
                                 : m_Look->Environments[static_cast<usize>(env - 1)].second;
                }
                UI::Tooltip("The environment lighting the scene");
            }

            UI::SameLine();
            if (UI::Button("..."))
            {
                UI::OpenPopup(SettingsPopup);
            }
            UI::Tooltip("Rendering settings");
            DrawSettings();
        }
    }

    void SceneViewportPanel::DrawSettings()
    {
        const auto popup = UI::Popup(SettingsPopup);
        if (!popup)
        {
            return;
        }

        // Lighting is the editor's own, so only a viewport under a preview look offers it; a
        // level's is its render block and sky, edited in its settings panel.
        if (m_Look != nullptr)
        {
            SceneLighting& l = m_Lighting;
            UI::SeparatorText("Lighting");
            (void)UI::Slider("Exposure", l.Exposure,
                             {.Min = -4.0f, .Max = 4.0f, .Format = "%.2f EV"});
            if (!m_OwnSky)
            {
                (void)UI::Slider("Environment Intensity", l.EnvIntensity,
                                 {.Min = 0.0f, .Max = 4.0f, .Format = "%.2f"});
            }
            (void)UI::Checkbox("Sun", l.Sun);
            if (l.Sun)
            {
                f32 yaw = glm::degrees(l.SunYaw);
                if (UI::Slider("Sun Azimuth", yaw,
                               {.Min = -180.0f, .Max = 180.0f, .Format = "%.0f deg"}))
                {
                    l.SunYaw = glm::radians(yaw);
                }
                f32 pitch = glm::degrees(l.SunPitch);
                if (UI::Slider("Sun Elevation", pitch,
                               {.Min = 0.0f, .Max = 90.0f, .Format = "%.0f deg"}))
                {
                    l.SunPitch = glm::radians(pitch);
                }
                (void)UI::Slider("Sun Intensity", l.SunIntensity,
                                 {.Min = 0.0f, .Max = 20.0f, .Format = "%.1f"});
                (void)UI::ColorEdit3("Sun Colour", l.SunColor);
            }
        }

        // Each toggle below is a Configure recompile, deferred via m_SettingsDirty to OnUI.
        UI::SeparatorText("Post");
        m_SettingsDirty |= UI::Checkbox("Bloom", m_Settings.Bloom);
        if (m_Settings.Bloom)
        {
            (void)UI::Slider("Bloom Strength", m_BaseView.BloomIntensity,
                             {.Min = 0.0f, .Max = 2.0f, .Format = "%.2f"});
            (void)UI::Slider("Bloom Radius", m_BaseView.BloomRadius,
                             {.Min = 0.25f, .Max = 3.0f, .Format = "%.2f"});
        }
        m_SettingsDirty |= UI::Checkbox("Ambient Occlusion", m_Settings.AO);
        i32 aa = static_cast<i32>(m_Settings.AntiAliasing);
        if (UI::Combo("Anti-aliasing", aa, Renderer::AntiAliasingModeNames))
        {
            m_Settings.AntiAliasing = static_cast<Renderer::AntiAliasingMode>(aa);
            m_SettingsDirty = true;
        }
        m_SettingsDirty |= UI::Checkbox("Depth of Field", m_Settings.DepthOfField);

        // Directional is the cascade, punctual the point/spot atlas; a scene's shadows can come
        // from either.
        UI::SeparatorText("Shadows");
        m_SettingsDirty |= UI::Checkbox("Directional", m_Settings.Shadows);
        m_SettingsDirty |= UI::Checkbox("Punctual", m_Settings.PunctualShadows);

        UI::SeparatorText("Camera");
        f32 fov = glm::degrees(m_Camera.GetFovY());
        if (UI::Slider("Field of View", fov, {.Min = 15.0f, .Max = 110.0f, .Format = "%.0f deg"}))
        {
            m_Camera.SetFovY(glm::radians(fov));
        }
        if (UI::Button("Reset All"))
        {
            ResetAll();
        }
    }

    void SceneViewportPanel::DrawCaptureNotice()
    {
        const UI::Theme& theme = UI::GetTheme();
        if (const auto banner =
                UI::ViewportOverlay("##capture-notice", UI::OverlayAnchor::TopCenter))
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

        // Play mouse capture is the router's gameplay focus, held by the document's token (pushed on
        // Play, popped by the document's Shift+Esc, suspended across a window-focus loss). Clicking
        // the viewport while playing re-grabs it after a release; recompute focus after so the push
        // takes effect this frame.
        if (m_Ctx.IsPlaying() && !m_Router.IsFocusTokenLive(m_Ctx.PlayCapture) && hovered &&
            m_Input.WasMouseButtonPressed(MouseButton::Left))
        {
            m_Ctx.PlayCapture = m_Router.PushFocus(InputFocus::Gameplay);
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

        // The editor camera reads viewport input only in edit mode. While a game is running it
        // stands down for the whole session — not just while the game holds focus, but also after
        // the player releases the cursor with Shift+Esc to click the editor UI; the released cursor
        // returns to the UI, never to the editor camera. In edit mode the camera reads input and
        // drives its own transient navigation cursor lock for the RMB-fly drag.
        // A click the camera consumed is navigation, not a selection: an RMB-fly, an MMB-pan, or an
        // Alt-orbit drag. Tracked so HandleClickToSelect only picks on a bare click.
        bool cameraConsumed = gameplayFocused;
        if (m_Ctx.IsPlaying())
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

        ApplyPreviewLighting();

        Renderer::ViewState view = m_BaseView;
        view.World = m_Ctx.Scene;
        view.Camera = camera;
        view.Delta = Time::GetDeltaTime();
        // Interpolate the play clone between its last two Sim ticks (zero while editing), so Play
        // renders as smoothly as the launcher above the tick rate.
        view.Alpha = m_Ctx.IsPlaying() ? m_Ctx.PlayAlpha : 0.0f;
        view.Exposure = m_BaseView.Exposure * std::exp2(m_Lighting.Exposure);

        // The one site the defocus parameters resolve: a Physical camera's lens wins over the
        // stored knobs, the CoC scale is derived from the target height, and the two quality knobs
        // are clamped. The panel keeps the report so the level editor can grey the lens fields.
        Veng::ResolveDofViewState(view, static_cast<f32>(renderExtent.y));
        m_DofFromPhysicalCamera = view.DofFromPhysicalCamera;

        // The sky is the scene's Sky component, resolved by the renderer itself each Execute — so
        // adding or editing the component in the inspector switches the sky live with no editor
        // mapping code and no Configure.
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
