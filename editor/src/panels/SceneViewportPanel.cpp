#include "SceneViewportPanel.h"

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Assert.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/ImGui/ImGuiTexture.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Time.h>
#include <Veng/UI/UI.h>

#include <array>

namespace VengEditor
{
    using namespace Veng;

    namespace
    {
        // The hello-triangle pack's brick material and the sphere prefab. The
        // editor renders the same content the game ships.
        constexpr AssetId BrickMaterialId{0x3EBULL};
        constexpr AssetId SpherePrefabId{0xA123F30FD219F2D5ULL};

        const vec3 SpinAxis = glm::normalize(vec3(0.5f, 1.0f, 0.2f));
    }

    SceneViewportPanel::SceneViewportPanel(Renderer::Context& context, AssetManager& assets,
                                           ImGuiLayer& imgui, TypeRegistry& types) :
        m_Context(context), m_Assets(assets), m_ImGui(imgui), m_Types(types)
    {
        m_RenderExtent = context.GetInternalRenderExtent();

        m_SceneRenderer = Renderer::SceneRenderer::Create({
            .Context = context,
            .Assets = assets,
            .OutputFormat = context.GetOutputFormat(),
            .Extent = m_RenderExtent,
            .Settings = m_Settings,
        });

        BuildScene();

        // Edge clamping prevents sampling past the image boundary when the panel size
        // does not align to a texel.
        m_SceneSampler = Renderer::Sampler::Create(context, {
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
        m_Scene.reset();
        m_BrickMaterial = {};
        m_SceneRenderer.reset();
    }

    void SceneViewportPanel::BuildScene()
    {
        const AssetResult<AssetHandle<Material>> brick = m_Assets.LoadSync<Material>(BrickMaterialId);
        VE_ASSERT(brick.has_value(), "{}", brick.error().Detail);
        m_BrickMaterial = *brick;

        const Ref<Mesh> sphere = Mesh::Create(
            m_Context, Primitives::Icosphere(0.8f, 4, m_BrickMaterial), "Editor Sphere");

        m_Scene = Scene::Create(m_Types);

        const AssetResult<AssetHandle<Prefab>> prefab = m_Assets.LoadSync<Prefab>(SpherePrefabId);
        VE_ASSERT(prefab.has_value(), "{}", prefab.error().Detail);

        const vector<Entity> roots = prefab->Get()->SpawnInto(*m_Scene, m_Assets);
        VE_ASSERT(!roots.empty(), "prefab spawned no root entities");

        m_Scene->Get<MeshRenderer>(roots[0]).Mesh = m_Assets.Adopt(sphere);
        m_PrimaryEntity = roots[0];

        const Entity lightEntity = m_Scene->CreateEntity();
        m_Scene->Add<Light>(lightEntity) = Light{
            .Direction = glm::normalize(vec3(-0.4f, -0.7f, -0.5f)),
            .Color = vec3(1.0f, 1.0f, 1.0f),
            .Intensity = 1.5f,
        };

        const f32 aspect = static_cast<f32>(m_RenderExtent.x) / static_cast<f32>(m_RenderExtent.y);
        m_Camera.SetPerspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);
        m_Camera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));
    }

    void SceneViewportPanel::Render(Renderer::CommandBuffer& cmd)
    {
        // Resize and Configure both invalidate GetOutput(); rebind the ImGui texture
        // once after both are applied, before recording.
        bool outputInvalidated = false;

        // A pending resize from the previous frame's content region: recreate the
        // renderer at the new size before recording.
        if (m_PendingExtent.x != 0 && m_PendingExtent.y != 0 && m_PendingExtent != m_RenderExtent)
        {
            m_RenderExtent = m_PendingExtent;
            m_SceneRenderer->Resize(m_RenderExtent);

            const f32 aspect = static_cast<f32>(m_RenderExtent.x) / static_cast<f32>(m_RenderExtent.y);
            m_Camera.SetPerspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);

            outputInvalidated = true;
        }

        // Settings changed in OnImGui; apply before recording so the output reflects the new pass set.
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

        const f32 delta = Time::GetDeltaTime();

        // Spinner is a game type, not a compile-time symbol in libveng_editor; read
        // its speed reflectively so inspector edits to Spinner.Speed take effect
        // immediately. Entities without Spinner advance at the default rate.
        m_Scene->Each<Transform>([this, delta](Entity entity, Transform& transform)
        {
            f32 speed = 1.0f;
            m_Scene->ForEachComponent(entity, [this, &speed](TypeId id, void* component)
            {
                const TypeInfo& info = m_Types.Info(id);
                for (const FieldDescriptor& field : info.Fields)
                {
                    if (field.Class == FieldClass::Scalar
                        && field.Type == m_Types.IdOf<f32>()
                        && field.Name == "SpeedRadiansPerSec")
                    {
                        speed = *reinterpret_cast<const f32*>(static_cast<u8*>(component) + field.Offset);
                    }
                }
            });

            m_SpinAccum[entity.Index] += delta * speed;
            transform.Rotation = glm::angleAxis(m_SpinAccum[entity.Index], SpinAxis);
        });

        const Renderer::SceneView view{.World = *m_Scene, .Camera = m_Camera, .Delta = delta};
        m_SceneRenderer->Execute(cmd, view);

        // ImGui's sampled read of the output is recorded outside the graph by
        // ImGuiLayer::Render, so no pass .Sample() covers it; transition the output
        // to a sampleable layout here, before that read.
        cmd.PrepareForAccess(m_SceneRenderer->GetOutput(), Renderer::AccessKind::Sample);
    }

    void SceneViewportPanel::OnImGui()
    {
        // Combo index is the DebugView enum value (declaration order). Change is
        // deferred via m_SettingsDirty so Configure runs before recording, not mid-ImGui.
        static constexpr std::array<string_view, 10> modeNames{
            "Final", "Albedo", "Normal", "Depth",
            "Roughness", "Metallic", "Occlusion",
            "AO", "Shadows", "Cascades"};
        i32 mode = static_cast<i32>(m_Settings.Mode);
        if (UI::Combo("Debug View", mode, modeNames))
        {
            m_Settings.Mode = static_cast<Renderer::DebugView>(mode);
            m_SettingsDirty = true;
        }

        // The image fills the region left below the dropdown; its size drives the
        // renderer's resize (applied next Render).
        const vec2 available = UI::ContentRegionAvail();
        const uvec2 wanted{static_cast<u32>(available.x), static_cast<u32>(available.y)};
        m_PendingExtent = wanted;

        UI::Image(m_SceneTexture, available);
    }
}
