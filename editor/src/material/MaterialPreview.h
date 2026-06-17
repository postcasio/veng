#pragma once

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Veng.h>

#include <imgui.h>

namespace Veng
{
    class AssetManager;
    class ImGuiLayer;
    class ImGuiTexture;
    class Material;
    class Mesh;
    class Scene;
    class TypeRegistry;

    namespace Renderer
    {
        class Context;
        class CommandBuffer;
        class Sampler;
        class SceneRenderer;
    }
}

namespace VengEditor
{
    // A reusable preview surface: one material on a sphere, rendered into a small
    // fixed-extent SceneRenderer target and shown via ImGui::Image. It owns a
    // one-sphere Scene (a lit stage — the sphere under a Transform + Camera, plus a
    // separate directional-Light entity), the SceneRenderer, and the Ref<ImGuiTexture>
    // the panel draws. SetMaterial swaps the previewed material after a recook hands
    // back a fresh handle.
    //
    // It records the scene render itself, so the host hands it the frame's command
    // buffer (Render) before the ImGui frame is built — the SceneViewportPanel
    // pattern, scoped to a single material on a sphere.
    //
    // The surface depends only on the shipped engine (Primitives, SceneRenderer,
    // Material, ImGuiLayer); it knows nothing of the node graph.
    class MaterialPreview
    {
    public:
        MaterialPreview(Veng::Renderer::Context& context, Veng::AssetManager& assets,
                        Veng::ImGuiLayer& imgui, Veng::uvec2 extent);
        ~MaterialPreview();

        MaterialPreview(const MaterialPreview&) = delete;
        MaterialPreview& operator=(const MaterialPreview&) = delete;

        // Swap the previewed material (after a recook hands back a fresh handle).
        void SetMaterial(Veng::AssetHandle<Veng::Material> material);

        // Record this frame's scene render. Called before the ImGui frame is built so
        // the output is ready for the ImGui::Image sample. Reads Time::GetDeltaTime()
        // internally — no delta parameter.
        void Render(Veng::Renderer::CommandBuffer& cmd);

        // The preview image for ImGui::Image, off the owned Ref<ImGuiTexture>.
        [[nodiscard]] ImTextureID GetTextureId() const;

        // The fixed preview extent the SceneRenderer is sized to.
        [[nodiscard]] Veng::uvec2 GetExtent() const { return m_Extent; }

        // Recreate the SceneRenderer at a new extent and re-register the output's
        // ImGuiTexture (Resize invalidates GetOutput()).
        void Resize(Veng::uvec2 extent);

    private:
        void BuildScene();
        void RegisterOutput();

        Veng::Renderer::Context& m_Context;
        Veng::AssetManager& m_Assets;
        Veng::ImGuiLayer& m_ImGui;

        // The preview owns its own TypeRegistry: the Scene it renders is private and
        // carries only the builtin components (Transform/Camera/MeshRenderer/Light),
        // so it needs no host registry.
        Veng::Unique<Veng::TypeRegistry> m_Types;
        Veng::Unique<Veng::Scene> m_Scene;
        Veng::Unique<Veng::Renderer::SceneRenderer> m_SceneRenderer;
        Veng::Camera m_Camera;

        // The sphere's owning mesh, kept resident for the renderer's lifetime, and
        // the sphere entity whose MeshRenderer material SetMaterial swaps.
        Veng::Ref<Veng::Mesh> m_Sphere;
        Veng::Entity m_SphereEntity;

        Veng::AssetHandle<Veng::Material> m_Material;

        Veng::Ref<Veng::Renderer::Sampler> m_Sampler;
        Veng::Ref<Veng::ImGuiTexture> m_Texture;

        Veng::uvec2 m_Extent{};

        // Accumulated turntable spin, advanced each Render by the frame delta.
        Veng::f32 m_SpinAccum = 0.0f;
    };
}
