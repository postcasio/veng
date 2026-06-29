#pragma once

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Veng.h>

namespace Veng
{
    class AssetManager;
    class ImGuiLayer;
    class ImGuiTexture;
    class MaterialInstance;
    class Mesh;
    class Scene;
    class TypeRegistry;

    namespace Renderer
    {
        class Context;
        class Sampler;
    }
}

namespace VengEditor
{
    /// @brief Renders one material on a sphere through an Offscreen viewport at a fixed extent.
    ///
    /// Owns a one-sphere Scene (sphere under Transform + MeshRenderer, plus a directional
    /// Light), an Offscreen Veng::Renderer::Viewport, and the ImGuiTexture the panel draws.
    /// SetMaterial swaps the previewed material after a recook hands back a fresh handle.
    ///
    /// The viewport is rendered by the engine drive-list (its owning MaterialEditorPanel
    /// registers it on this preview's behalf — MaterialPreview is not an EditorPanel). Each
    /// frame the preview advances the turntable and pushes the view through Update; the
    /// engine renders the registered viewport, and GetTexture() samples the result.
    /// The class depends only on the shipped engine and knows nothing of the node graph.
    class MaterialPreview
    {
    public:
        /// @brief Constructs the preview at the given @p extent.
        /// @param context  Renderer context owning all GPU resources.
        /// @param assets   Asset manager used to adopt the sphere mesh.
        /// @param imgui    ImGui layer used to register the output texture.
        /// @param extent   Render resolution in pixels.
        MaterialPreview(Veng::Renderer::Context& context, Veng::AssetManager& assets,
                        Veng::ImGuiLayer& imgui, Veng::uvec2 extent);
        ~MaterialPreview();

        MaterialPreview(const MaterialPreview&) = delete;
        MaterialPreview& operator=(const MaterialPreview&) = delete;

        /// @brief Returns the owned viewport so the owning panel can register it.
        [[nodiscard]] Veng::Renderer::Viewport& GetViewport() const { return *m_Viewport; }

        /// @brief Swaps the previewed material.
        /// @param material  Fresh handle returned by a recook.
        void SetMaterial(Veng::AssetHandle<Veng::MaterialInstance> material);

        /// @brief Advances the turntable and pushes this frame's view onto the viewport.
        ///
        /// Reads Time::GetDeltaTime() internally. The engine drive-list renders the registered
        /// viewport; this records no scene render itself.
        void Update();

        /// @brief Returns the ImGuiTexture for use with UI::Image.
        [[nodiscard]] const Veng::Ref<Veng::ImGuiTexture>& GetTexture() const;

        /// @brief Returns the current render extent.
        [[nodiscard]] Veng::uvec2 GetExtent() const { return m_Extent; }

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
        /// @brief The Offscreen viewport rendering the preview; registered by the owning panel.
        Veng::Unique<Veng::Renderer::Viewport> m_Viewport;
        /// @brief Preview camera (fixed perspective).
        Veng::CameraView m_Camera;

        /// @brief Owned sphere mesh, kept resident for the renderer's lifetime.
        Veng::Ref<Veng::Mesh> m_Sphere;
        /// @brief Entity whose MeshRenderer material SetMaterial swaps.
        Veng::Entity m_SphereEntity;

        /// @brief The currently previewed material handle.
        Veng::AssetHandle<Veng::MaterialInstance> m_Material;

        /// @brief Sampler for the preview output (edge clamp).
        Veng::Ref<Veng::Renderer::Sampler> m_SceneSampler;
        /// @brief ImGuiTexture over the viewport's output.
        Veng::Ref<Veng::ImGuiTexture> m_SceneTexture;

        /// @brief Render resolution.
        Veng::uvec2 m_Extent{};

        /// @brief Accumulated turntable spin angle, advanced each Update by the frame delta.
        Veng::f32 m_SpinAccum = 0.0f;
    };
}
