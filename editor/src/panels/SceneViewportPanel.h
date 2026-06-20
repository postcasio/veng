#pragma once

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Scene.h>

#include <VengEditor/EditorPanel.h>

namespace Veng
{
    class AssetManager;
    class ImGuiLayer;
    class ImGuiTexture;
    class TypeRegistry;
    class Material;

    namespace Renderer
    {
        class Context;
        class CommandBuffer;
        class Sampler;
    }
}

namespace VengEditor
{
    /// @brief Scene viewport panel: owns a SceneRenderer, records and displays the
    /// scene each frame in a UI::Image.
    ///
    /// The image resizes to the panel's content region (debounced — SceneRenderer
    /// is recreated only when the size settles). The host must call Render() with
    /// the frame's command buffer before OnImGui so the output is ready.
    class SceneViewportPanel final : public EditorPanel
    {
    public:
        /// @brief Constructs the viewport, creating a SceneRenderer at the context's
        /// current internal render extent.
        SceneViewportPanel(Veng::Renderer::Context& context, Veng::AssetManager& assets,
                           Veng::ImGuiLayer& imgui, Veng::TypeRegistry& types);
        ~SceneViewportPanel() override;

        [[nodiscard]] Veng::string_view GetTitle() const override { return "Scene Viewport"; }
        [[nodiscard]] Veng::UI::WindowFlags GetWindowFlags() const override
        {
            return Veng::UI::WindowFlags::NoScrollbar | Veng::UI::WindowFlags::NoScrollWithMouse;
        }

        /// @brief Records this frame's scene render.
        /// @pre Called by the host before OnImGui so the output is ready for UI::Image.
        void Render(Veng::Renderer::CommandBuffer& cmd);

        /// @brief Returns the scene the viewport renders; read by the inspector.
        [[nodiscard]] Veng::Scene& GetScene() const { return *m_Scene; }

        /// @brief Returns the prefab's spawned root entity (the inspector's default selection).
        [[nodiscard]] Veng::optional<Veng::Entity> PrimaryEntity() const { return m_PrimaryEntity; }

        void OnImGui() override;

    private:
        /// @brief Loads assets, spawns the prefab, and initialises the camera.
        void BuildScene();

        Veng::Renderer::Context& m_Context;
        Veng::AssetManager& m_Assets;
        Veng::ImGuiLayer& m_ImGui;
        Veng::TypeRegistry& m_Types;

        Veng::Unique<Veng::Renderer::SceneRenderer> m_SceneRenderer;
        Veng::Unique<Veng::Scene> m_Scene;
        Veng::Camera m_Camera;

        Veng::AssetHandle<Veng::Material> m_BrickMaterial;

        /// @brief ImGui texture over SceneRenderer::GetOutput(); recreated when Resize
        /// or Configure invalidates the output.
        Veng::Ref<Veng::Renderer::Sampler> m_SceneSampler;
        Veng::Ref<Veng::ImGuiTexture> m_SceneTexture;

        Veng::optional<Veng::Entity> m_PrimaryEntity;

        /// @brief Renderer settings driven by the debug-view dropdown.
        ///
        /// A change from OnImGui sets m_SettingsDirty; Render applies it through
        /// Configure before recording, so the rebound output is the freshly-rendered one.
        Veng::Renderer::SceneRendererSettings m_Settings;
        bool m_SettingsDirty = false;

        Veng::uvec2 m_RenderExtent{};
        Veng::uvec2 m_PendingExtent{};

        /// @brief Per-entity accumulated spin angle (keyed by entity index).
        Veng::map<Veng::u32, Veng::f32> m_SpinAccum;
    };
}
