#include "MaterialPreview.h"

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Assert.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/ImGui/ImGuiTexture.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/ImGuiCompositePass.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Time.h>

namespace VengEditor
{
    using namespace Veng;

    namespace
    {
        // The turntable spin axis (a slight tilt off vertical) and rate.
        const vec3 SpinAxis = glm::normalize(vec3(0.0f, 1.0f, 0.0f));
        constexpr f32 SpinSpeed = 0.6f;
    }

    MaterialPreview::MaterialPreview(Renderer::Context& context, AssetManager& assets,
                                     ImGuiLayer& imgui, uvec2 extent) :
        m_Context(context), m_Assets(assets), m_ImGui(imgui), m_Extent(extent)
    {
        m_SceneRenderer = Renderer::SceneRenderer::Create({
            .Context = context,
            .Assets = assets,
            .OutputFormat = context.GetOutputFormat(),
            .Extent = m_Extent,
            .Settings = {},
        });

        BuildScene();

        // Panel-only mode: the pass owns the ImGui preview texture and the
        // pre-Render barrier; the preview shows inside an ImGui::Image.
        m_Composite = Renderer::ImGuiCompositePass::Create({
            .Context = context,
            .ImGui = imgui,
            .Assets = assets,
            .SceneSource = m_SceneRenderer->GetOutput(),
        });
    }

    MaterialPreview::~MaterialPreview()
    {
        m_Composite.reset();
        m_Scene.reset();
        m_Sphere.reset();
        m_Material = {};
        m_SceneRenderer.reset();
        m_Types.reset();
    }

    void MaterialPreview::BuildScene()
    {
        m_Types = CreateUnique<TypeRegistry>();
        RegisterBuiltinTypes(*m_Types);

        m_Scene = Scene::Create(*m_Types);

        // A materialless sphere; SetMaterial assigns the previewed material's owning
        // mesh once a handle is available.
        m_Sphere = Mesh::Create(m_Context, Primitives::Icosphere(0.85f, 4), "Material Preview Sphere");

        m_SphereEntity = m_Scene->CreateEntity();
        m_Scene->Add<Transform>(m_SphereEntity);
        m_Scene->Add<MeshRenderer>(m_SphereEntity).Mesh = m_Assets.Adopt(m_Sphere);

        const Entity lightEntity = m_Scene->CreateEntity();
        m_Scene->Add<Light>(lightEntity) = Light{
            .Direction = glm::normalize(vec3(-0.4f, -0.7f, -0.5f)),
            .Color = vec3(1.0f, 1.0f, 1.0f),
            .Intensity = 1.5f,
        };

        const f32 aspect = static_cast<f32>(m_Extent.x) / static_cast<f32>(m_Extent.y);
        m_Camera.SetPerspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);
        m_Camera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));
    }

    void MaterialPreview::SetMaterial(AssetHandle<Material> material)
    {
        m_Material = std::move(material);

        // Rebuild the sphere on the new material so the mesh owns it and the draw
        // loop binds it; the old mesh retires through the per-frame path.
        m_Sphere = Mesh::Create(m_Context, Primitives::Icosphere(0.85f, 4, m_Material),
                                "Material Preview Sphere");
        m_Scene->Get<MeshRenderer>(m_SphereEntity).Mesh = m_Assets.Adopt(m_Sphere);
    }

    void MaterialPreview::Render(Renderer::CommandBuffer& cmd)
    {
        const f32 delta = Time::GetDeltaTime();
        m_SpinAccum += delta * SpinSpeed;
        m_Scene->Get<Transform>(m_SphereEntity).Rotation = glm::angleAxis(m_SpinAccum, SpinAxis);

        const Renderer::SceneView view{.World = *m_Scene, .Camera = m_Camera, .Delta = delta};
        m_SceneRenderer->Execute(cmd, view);

        // ImGui samples the output outside the graph; the composite pass issues the
        // sampleability barrier before ImGuiLayer::Render records the read. The
        // renderer re-arms ColorAttachment before its next Execute.
        m_Composite->PrepareSceneForImGui(cmd);
    }

    ImTextureID MaterialPreview::GetTextureId() const
    {
        return static_cast<ImTextureID>(m_Composite->GetSceneTexture().GetTextureId());
    }

    void MaterialPreview::Resize(uvec2 extent)
    {
        if (extent.x == 0 || extent.y == 0 || extent == m_Extent)
            return;

        m_Extent = extent;
        m_SceneRenderer->Resize(m_Extent);

        const f32 aspect = static_cast<f32>(m_Extent.x) / static_cast<f32>(m_Extent.y);
        m_Camera.SetPerspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);

        // Resize invalidates GetOutput(), so re-bind the composite pass's source.
        m_Composite->SetSource(m_SceneRenderer->GetOutput());
    }
}
