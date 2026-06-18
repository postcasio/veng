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
#include <Veng/Renderer/ImGuiCompositePass.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Time.h>
#include <Veng/UI/UI.h>

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
            .Settings = {},
        });

        BuildScene();

        // Panel-only mode (no SwapChainFormat): the pass owns the ImGui scene
        // texture and the pre-Render barrier; the scene shows inside this panel.
        m_Composite = Renderer::ImGuiCompositePass::Create({
            .Context = context,
            .ImGui = imgui,
            .Assets = assets,
            .SceneSource = m_SceneRenderer->GetOutput(),
        });
    }

    SceneViewportPanel::~SceneViewportPanel()
    {
        m_Composite.reset();
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
        // A pending resize from the previous frame's content region: recreate the
        // renderer at the new size before recording, then re-bind the source
        // (Resize invalidates GetOutput()).
        if (m_PendingExtent.x != 0 && m_PendingExtent.y != 0 && m_PendingExtent != m_RenderExtent)
        {
            m_RenderExtent = m_PendingExtent;
            m_SceneRenderer->Resize(m_RenderExtent);

            const f32 aspect = static_cast<f32>(m_RenderExtent.x) / static_cast<f32>(m_RenderExtent.y);
            m_Camera.SetPerspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);
            m_Composite->SetSource(m_SceneRenderer->GetOutput());
        }

        const f32 delta = Time::GetDeltaTime();

        // Spin every entity carrying a Transform, advancing each by its Spinner's
        // speed. The game's Spinner type is not a compile-time symbol in
        // libveng_editor, so the viewport reads its speed reflectively (by field
        // name through the TypeRegistry) — which is what makes editing Spinner.Speed
        // in the inspector change the visible rotation rate. An entity without a
        // Spinner (or one whose field is absent) advances at the default rate.
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

        // ImGui samples the output outside the graph; the composite pass issues the
        // sampleability barrier before ImGuiLayer::Render records the read.
        m_Composite->PrepareSceneForImGui(cmd);
    }

    void SceneViewportPanel::OnImGui()
    {
        const vec2 available = UI::ContentRegionAvail();
        const uvec2 wanted{static_cast<u32>(available.x), static_cast<u32>(available.y)};
        m_PendingExtent = wanted;

        UI::Image(m_Composite->GetSceneTexture(), available);
    }
}
