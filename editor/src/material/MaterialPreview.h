#pragma once

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Veng.h>

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
    /// @brief Renders one material on a sphere into a fixed-extent SceneRenderer target.
    ///
    /// Owns a one-sphere Scene (sphere under Transform + MeshRenderer, plus a
    /// directional Light), the SceneRenderer, and the ImGuiTexture the panel draws.
    /// SetMaterial swaps the previewed material after a recook hands back a fresh handle.
    ///
    /// Render records the scene render itself; the host passes the frame's command
    /// buffer before the ImGui frame is built so the output is ready for UI::Image.
    /// The class depends only on the shipped engine and knows nothing of the node graph.
    class MaterialPreview
    {
    public:
        /// @brief Constructs the preview at the given @p extent.
        /// @param context  Renderer context owning all GPU resources.
        /// @param assets   Asset manager used to adopt the sphere mesh.
        /// @param imgui    ImGui layer used to register the output texture.
        /// @param extent   Initial render resolution in pixels.
        MaterialPreview(Veng::Renderer::Context& context, Veng::AssetManager& assets,
                        Veng::ImGuiLayer& imgui, Veng::uvec2 extent);
        ~MaterialPreview();

        MaterialPreview(const MaterialPreview&) = delete;
        MaterialPreview& operator=(const MaterialPreview&) = delete;

        /// @brief Swaps the previewed material.
        /// @param material  Fresh handle returned by a recook.
        void SetMaterial(Veng::AssetHandle<Veng::Material> material);

        /// @brief Records this frame's scene render into @p cmd.
        ///
        /// Must be called before the ImGui frame is built so the output texture is
        /// ready for UI::Image. Reads Time::GetDeltaTime() internally.
        void Render(Veng::Renderer::CommandBuffer& cmd);

        /// @brief Returns the ImGuiTexture for use with UI::Image.
        [[nodiscard]] const Veng::Ref<Veng::ImGuiTexture>& GetTexture() const;

        /// @brief Returns the current render extent.
        [[nodiscard]] Veng::uvec2 GetExtent() const { return m_Extent; }

        /// @brief Recreates the SceneRenderer at a new extent and re-registers the ImGuiTexture.
        ///
        /// Resize invalidates GetOutput(); the ImGuiTexture is rebuilt over the new view.
        void Resize(Veng::uvec2 extent);

    private:
        /// @brief Constructs the one-sphere scene and camera.
        void BuildScene();

        Veng::Renderer::Context& m_Context;
        Veng::AssetManager& m_Assets;
        Veng::ImGuiLayer& m_ImGui;

        /// @brief Private TypeRegistry for the preview scene's builtin components.
        ///
        /// The scene is not shared with the host, so it needs no host registry.
        Veng::Unique<Veng::TypeRegistry> m_Types;
        /// @brief The one-sphere preview scene.
        Veng::Unique<Veng::Scene> m_Scene;
        /// @brief SceneRenderer sized to the preview extent.
        Veng::Unique<Veng::Renderer::SceneRenderer> m_SceneRenderer;
        /// @brief Preview camera (fixed perspective, updated on Resize).
        Veng::Camera m_Camera;

        /// @brief Owned sphere mesh, kept resident for the renderer's lifetime.
        Veng::Ref<Veng::Mesh> m_Sphere;
        /// @brief Entity whose MeshRenderer material SetMaterial swaps.
        Veng::Entity m_SphereEntity;

        /// @brief The currently previewed material handle.
        Veng::AssetHandle<Veng::Material> m_Material;

        /// @brief Sampler for the preview output (edge clamp).
        Veng::Ref<Veng::Renderer::Sampler> m_SceneSampler;
        /// @brief ImGuiTexture over SceneRenderer::GetOutput(); recreated on Resize.
        Veng::Ref<Veng::ImGuiTexture> m_SceneTexture;

        /// @brief Current render resolution.
        Veng::uvec2 m_Extent{};

        /// @brief Accumulated turntable spin angle, advanced each Render by the frame delta.
        Veng::f32 m_SpinAccum = 0.0f;
    };
}
