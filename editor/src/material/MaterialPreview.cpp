#include "MaterialPreview.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <glm/gtc/quaternion.hpp>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Environment.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/ImGui/ImGuiTexture.h>
#include <Veng/Log.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneViewport.h>
#include <Veng/Time.h>
#include <Veng/UI/Layout.h>
#include <Veng/UI/Query.h>
#include <Veng/UI/Scopes.h>
#include <Veng/UI/Widgets.h>

namespace VengEditor
{
    using namespace Veng;

    namespace
    {
        // A sun intensity of 1 in the controls is this illuminance: a bright overcast day.
        constexpr f32 LuxPerSunUnit = 10000.0f;

        constexpr std::array<string_view, static_cast<usize>(MaterialPreviewShape::Count)>
            ShapeNames{"Sphere", "Cube", "Plane", "Cylinder", "Torus"};

        // The views a material is judged by: the lit result, then each g-buffer channel it writes.
        constexpr std::array<Renderer::DebugView, 7> Views{
            Renderer::DebugView::Final,    Renderer::DebugView::Albedo,
            Renderer::DebugView::Normal,   Renderer::DebugView::Roughness,
            Renderer::DebugView::Metallic, Renderer::DebugView::Occlusion,
            Renderer::DebugView::Emissive};
        constexpr std::array<string_view, 7> ViewNames{
            "Lit", "Albedo", "Normal", "Roughness", "Metallic", "Occlusion", "Emissive"};

        // The direction from the origin toward a point `yaw` around +Y from +X and `pitch` up.
        vec3 OrbitDirection(f32 yaw, f32 pitch)
        {
            const f32 c = std::cos(pitch);
            return {c * std::cos(yaw), std::sin(pitch), c * std::sin(yaw)};
        }

        // A plane lies in xz facing +y. The preview stands it up facing +x, the camera at yaw 0,
        // like a swatch: its u running to the screen's right (-z) and v down it (-y).
        quat ShapeRest(MaterialPreviewShape shape)
        {
            if (shape != MaterialPreviewShape::Plane)
            {
                return quat{1.0f, 0.0f, 0.0f, 0.0f};
            }
            return glm::quat_cast(mat3{vec3{0, 0, -1}, vec3{1, 0, 0}, vec3{0, -1, 0}});
        }

        MeshData ShapeMesh(MaterialPreviewShape shape, f32 tiling,
                           AssetHandle<MaterialInstance> material)
        {
            MeshData data;
            switch (shape)
            {
            case MaterialPreviewShape::Cube:
                data = Primitives::Cube(1.2f, std::move(material));
                break;
            case MaterialPreviewShape::Plane:
                data = Primitives::Plane(vec2{2.0f}, uvec2{1}, std::move(material));
                break;
            case MaterialPreviewShape::Cylinder:
                data = Primitives::Cylinder(0.65f, 1.4f, 64, std::move(material));
                break;
            case MaterialPreviewShape::Torus:
                data = Primitives::Torus(0.7f, 0.3f, 96, 48, std::move(material));
                break;
            default:
                data = Primitives::Sphere(0.9f, 64, 128, std::move(material));
                break;
            }
            // A generic material has no tiling of its own, so the mesh's UVs repeat instead.
            for (CanonicalVertex& v : data.Vertices)
            {
                v.UV *= tiling;
            }
            return data;
        }

        template <typename T, usize N>
        i32 IndexOf(const std::array<T, N>& items, const T& value)
        {
            const auto it = std::ranges::find(items, value);
            return it == items.end() ? 0 : static_cast<i32>(it - items.begin());
        }
    }

    MaterialPreviewState MaterialPreview::DefaultState(const PreviewLook& look)
    {
        MaterialPreviewState s;
        s.Environment = look.Environment;
        s.FovY = look.FovY;
        // With no environment to light it, the preview is lit by a sun instead.
        s.Sun = !look.Environment.IsValid();
        s.SunIntensity = s.Sun ? 10.0f : 3.0f;
        const Renderer::ViewState view;
        s.Bloom = look.Render ? look.Render->Bloom : true;
        s.BloomStrength = look.Render ? look.Render->BloomIntensity : view.BloomIntensity;
        s.BloomRadius = look.Render ? look.Render->BloomRadius : view.BloomRadius;
        return s;
    }

    MaterialPreview::MaterialPreview(Renderer::Context& context, AssetManager& assets,
                                     ImGuiLayer& imgui, uvec2 extent, const PreviewLook& look)
        : m_Context(context), m_Assets(assets), m_ImGui(imgui), m_Look(look), m_Extent(extent),
          m_State(DefaultState(look))
    {
        // The look's render block over the renderer's defaults: the settings and view the
        // controls then adjust.
        if (look.Render)
        {
            ApplyLevelRenderSettings(*look.Render, m_BaseSettings, m_BaseView);
        }
        m_Configured = m_BaseSettings;
        m_Viewport = Renderer::Viewport::Create({
            .Context = context,
            .Assets = assets,
            .Region = {.Offset = {0, 0}, .Extent = m_Extent},
            .ColorFormat = context.GetOutputFormat(),
            .Settings = m_Configured,
            .Role = Renderer::ViewportRole::Offscreen,
            // Render only while the material editor draws; a hidden tab pushes no ViewState, so the
            // engine skips the preview rather than rendering it behind the visible editor.
            .RenderOnDemand = true,
        });

        BuildScene();

        // Edge clamping prevents sampling past the preview image boundary.
        m_SceneSampler = Renderer::Sampler::Create(
            context, {
                         .Name = "Material Preview Sampler",
                         .AddressModeU = Renderer::AddressMode::ClampToEdge,
                         .AddressModeV = Renderer::AddressMode::ClampToEdge,
                         .AddressModeW = Renderer::AddressMode::ClampToEdge,
                     });
        m_ShownOutput = m_Viewport->GetOutput();
        m_SceneTexture = imgui.CreateTexture(*m_SceneSampler, *m_ShownOutput);
    }

    MaterialPreview::~MaterialPreview()
    {
        m_SceneTexture.reset();
        m_SceneSampler.reset();
        m_Scene.reset();
        m_Shape.reset();
        m_Material = {};
        m_Environment = {};
        m_Viewport.reset();
        m_Types.reset();
    }

    void MaterialPreview::BuildScene()
    {
        m_Types = CreateUnique<TypeRegistry>();
        RegisterBuiltinTypes(*m_Types);
        m_Scene = Scene::Create(*m_Types);

        m_ShapeEntity = m_Scene->CreateEntity();
        m_Scene->Add<Transform>(m_ShapeEntity);
        m_Scene->Add<MeshRenderer>(m_ShapeEntity);
        RebuildShape();

        m_SunEntity = m_Scene->CreateEntity();
        m_Scene->Add<Light>(m_SunEntity) = Light{.Color = vec3(1.0f)};

        m_SkyEntity = m_Scene->CreateEntity();
        m_Scene->Add<Sky>(m_SkyEntity).Lighting = SkyLighting::IBL;
    }

    void MaterialPreview::RebuildShape()
    {
        // Built with the material so the mesh owns it and the draw loop binds it; the old mesh
        // retires through the per-frame path.
        m_Shape = Mesh::BuildSync(m_Context, ShapeMesh(m_State.Shape, m_State.Tiling, m_Material),
                                  "Material Preview Shape");
        m_Scene->Get<MeshRenderer>(m_ShapeEntity).Mesh = m_Assets.Adopt(m_Shape);
        m_BuiltShape = m_State.Shape;
        m_BuiltTiling = m_State.Tiling;
    }

    void MaterialPreview::SetMaterial(AssetHandle<MaterialInstance> material)
    {
        m_Material = std::move(material);
        RebuildShape();
    }

    void MaterialPreview::Update()
    {
        const MaterialPreviewState& s = m_State;
        if (s.Shape != m_BuiltShape || s.Tiling != m_BuiltTiling)
        {
            RebuildShape();
        }

        // The environment the picker names, loaded when it changes.
        if (s.Environment != m_LoadedEnvironment)
        {
            m_LoadedEnvironment = s.Environment;
            m_Environment = {};
            if (s.Environment.IsValid())
            {
                if (auto loaded = m_Assets.LoadSync<EnvironmentMap>(s.Environment))
                {
                    m_Environment = *loaded;
                }
                else
                {
                    Log::Warn("Material preview: environment 0x{:X} did not load: {}",
                              s.Environment.Value, loaded.error().Detail);
                }
            }
        }
        Sky& sky = m_Scene->Get<Sky>(m_SkyEntity);
        sky.Intensity = s.EnvIntensity;
        if (m_Environment.Get() != nullptr)
        {
            auto* source =
                static_cast<EnvironmentSky*>(sky.Source.SetActive(TypeIdOf<EnvironmentSky>()));
            source->Map = m_Environment;
        }
        else
        {
            sky.Source = {};
        }

        // The environment can't rotate, so the shape, camera and sun turn the other way.
        const quat turn = glm::angleAxis(-s.EnvRotation, vec3{0.0f, 1.0f, 0.0f});
        m_Scene->Get<Transform>(m_ShapeEntity).Rotation = turn * ShapeRest(s.Shape);
        auto& sun = m_Scene->Get<Light>(m_SunEntity);
        sun.Type = LightType::Directional;
        sun.Direction = -(turn * OrbitDirection(s.SunYaw, s.SunPitch));
        sun.Color = s.SunColor;
        sun.Intensity = s.Sun ? s.SunIntensity * LuxPerSunUnit : 0.0f;

        const f32 aspect = static_cast<f32>(m_Extent.x) / static_cast<f32>(m_Extent.y);
        m_Camera.SetPerspective(s.FovY, aspect, 0.05f, 100.0f);
        m_Camera.SetView(OrbitDirection(s.Yaw - s.EnvRotation, s.Pitch) * s.Distance, vec3(0.0f),
                         vec3(0.0f, 1.0f, 0.0f));

        // The look's topology with the controls' bloom and view.
        Renderer::SceneRendererSettings settings = m_BaseSettings;
        settings.Bloom = s.Bloom;
        settings.Mode = s.View;
        if (settings.Bloom != m_Configured.Bloom || settings.Mode != m_Configured.Mode)
        {
            m_Viewport->Configure(settings);
            m_Configured = settings;
        }

        Renderer::ViewState view = m_BaseView;
        view.World = m_Scene.get();
        view.Camera = m_Camera;
        view.Delta = Time::GetDeltaTime();
        view.Exposure = m_BaseView.Exposure * std::exp2(s.Exposure);
        view.BloomIntensity = s.BloomStrength;
        view.BloomRadius = s.BloomRadius;
        m_Viewport->SetViewState(view);
    }

    void MaterialPreview::Draw(f32 width)
    {
        MaterialPreviewState& s = m_State;
        const f32 comboWidth = (width - 36.0f) / 3.0f;

        UI::SetNextItemWidth(comboWidth);
        i32 shape = static_cast<i32>(s.Shape);
        if (UI::Combo("##shape", shape, ShapeNames))
        {
            s.Shape = static_cast<MaterialPreviewShape>(shape);
        }
        UI::SameLine();
        UI::SetNextItemWidth(comboWidth);
        i32 view = IndexOf(Views, s.View);
        if (UI::Combo("##view", view, ViewNames))
        {
            s.View = Views[static_cast<usize>(view)];
        }
        UI::SameLine();
        UI::SetNextItemWidth(comboWidth);
        vector<string_view> envNames{"No Environment"};
        i32 env = 0;
        for (usize i = 0; i < m_Look.Environments.size(); ++i)
        {
            envNames.push_back(m_Look.Environments[i].first);
            if (m_Look.Environments[i].second == s.Environment)
            {
                env = static_cast<i32>(i + 1);
            }
        }
        if (UI::Combo("##env", env, envNames))
        {
            s.Environment =
                env == 0 ? AssetId{} : m_Look.Environments[static_cast<usize>(env - 1)].second;
        }
        UI::SameLine();
        if (UI::Button("..."))
        {
            UI::OpenPopup("PreviewSettings");
        }
        DrawSettings();

        // A reconfigure (bloom, the view) replaces the output at the next render: show the new one.
        if (m_Viewport->GetOutput() != m_ShownOutput)
        {
            m_ShownOutput = m_Viewport->GetOutput();
            m_SceneTexture = m_ImGui.CreateTexture(*m_SceneSampler, *m_ShownOutput);
        }
        UI::Image(m_SceneTexture, vec2(width, width));
        // A drag begun on the preview carries on until its buttons are released, wherever the
        // pointer goes: drag orbits, right-drag turns the environment (with Shift, the sun).
        const bool hovered = UI::ItemHovered();
        if (hovered && (UI::IsMouseClicked(UI::MouseButton::Left) ||
                        UI::IsMouseClicked(UI::MouseButton::Right)))
        {
            m_Dragging = true;
        }
        const bool left = UI::IsMouseDown(UI::MouseButton::Left);
        const bool right = UI::IsMouseDown(UI::MouseButton::Right);
        if (!left && !right)
        {
            m_Dragging = false;
        }
        if (m_Dragging)
        {
            const vec2 d = UI::MouseDelta();
            if (left)
            {
                s.Yaw += d.x * 0.008f;
                s.Pitch = std::clamp(s.Pitch + d.y * 0.008f, -1.5f, 1.5f);
            }
            if (right && UI::IsShiftDown())
            {
                s.SunYaw += d.x * 0.01f;
                s.SunPitch = std::clamp(s.SunPitch - d.y * 0.01f, 0.0f, 1.57f);
            }
            else if (right)
            {
                s.EnvRotation += d.x * 0.01f;
            }
        }
        if (hovered)
        {
            if (const f32 wheel = UI::MouseWheel(); wheel != 0.0f)
            {
                s.Distance = std::clamp(s.Distance * std::pow(0.9f, wheel), 1.3f, 12.0f);
            }
            if (UI::IsMouseDoubleClicked(UI::MouseButton::Left))
            {
                const MaterialPreviewState fresh = DefaultState(m_Look);
                s.Yaw = fresh.Yaw;
                s.Pitch = fresh.Pitch;
                s.Distance = fresh.Distance;
            }
            UI::Tooltip("Drag to orbit, wheel to zoom, right-drag to turn the environment "
                        "(Shift: the sun), double-click to reset the view");
        }
    }

    void MaterialPreview::DrawSettings()
    {
        const auto popup = UI::Popup("PreviewSettings");
        if (!popup)
        {
            return;
        }
        MaterialPreviewState& s = m_State;
        UI::SeparatorText("Surface");
        (void)UI::Slider("Tiling", s.Tiling, {.Min = 0.25f, .Max = 8.0f, .Format = "%.2f"});

        UI::SeparatorText("Lighting");
        (void)UI::Slider("Exposure", s.Exposure, {.Min = -4.0f, .Max = 4.0f, .Format = "%.2f EV"});
        (void)UI::Slider("Environment Intensity", s.EnvIntensity,
                         {.Min = 0.0f, .Max = 4.0f, .Format = "%.2f"});
        f32 rotation = glm::degrees(s.EnvRotation);
        if (UI::Slider("Environment Rotation", rotation,
                       {.Min = -180.0f, .Max = 180.0f, .Format = "%.0f deg"}))
        {
            s.EnvRotation = glm::radians(rotation);
        }
        (void)UI::Checkbox("Sun", s.Sun);
        if (s.Sun)
        {
            f32 yaw = glm::degrees(s.SunYaw);
            if (UI::Slider("Sun Azimuth", yaw,
                           {.Min = -180.0f, .Max = 180.0f, .Format = "%.0f deg"}))
            {
                s.SunYaw = glm::radians(yaw);
            }
            f32 pitch = glm::degrees(s.SunPitch);
            if (UI::Slider("Sun Elevation", pitch,
                           {.Min = 0.0f, .Max = 90.0f, .Format = "%.0f deg"}))
            {
                s.SunPitch = glm::radians(pitch);
            }
            (void)UI::Slider("Sun Intensity", s.SunIntensity,
                             {.Min = 0.0f, .Max = 20.0f, .Format = "%.1f"});
            (void)UI::ColorEdit3("Sun Colour", s.SunColor);
        }

        UI::SeparatorText("Post");
        (void)UI::Checkbox("Bloom", s.Bloom);
        if (s.Bloom)
        {
            (void)UI::Slider("Bloom Strength", s.BloomStrength,
                             {.Min = 0.0f, .Max = 2.0f, .Format = "%.2f"});
            (void)UI::Slider("Bloom Radius", s.BloomRadius,
                             {.Min = 0.25f, .Max = 3.0f, .Format = "%.2f"});
        }

        UI::SeparatorText("Camera");
        f32 fov = glm::degrees(s.FovY);
        if (UI::Slider("Field of View", fov, {.Min = 15.0f, .Max = 90.0f, .Format = "%.0f deg"}))
        {
            s.FovY = glm::radians(fov);
        }
        if (UI::Button("Reset View"))
        {
            const MaterialPreviewState fresh = DefaultState(m_Look);
            s.Yaw = fresh.Yaw;
            s.Pitch = fresh.Pitch;
            s.Distance = fresh.Distance;
            s.FovY = fresh.FovY;
        }
        UI::SameLine();
        if (UI::Button("Reset All"))
        {
            s = DefaultState(m_Look);
        }
    }

    const Ref<ImGuiTexture>& MaterialPreview::GetTexture() const
    {
        return m_SceneTexture;
    }
}
