#pragma once

#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Scene/Camera.h>

#include <VengEditor/EditorPanel.h>

#include "panels/PrefabEditContext.h"

namespace Veng
{
    class AssetManager;
    class ImGuiLayer;
    class ImGuiTexture;

    namespace Renderer
    {
        class Context;
        class CommandBuffer;
        class Sampler;
    }
}

namespace VengEditor
{
    /// @brief Scene viewport child of a prefab editor: renders the document's Scene from
    /// an orbit camera into a UI::Image.
    ///
    /// Owns a SceneRenderer sized to the panel's content region (debounced resize) and an
    /// orbit camera; reads the live Scene from the shared PrefabEditContext. The host
    /// records the scene render via OnRender before the ImGui frame is built, so the
    /// output is sampleable when OnImGui draws it.
    class SceneViewportPanel final : public EditorPanel
    {
    public:
        /// @brief Constructs the viewport over the document's edit context.
        /// @param context  Render context the SceneRenderer is created against.
        /// @param assets   Asset manager the renderer resolves materials through.
        /// @param imgui    ImGui layer the render target is registered with.
        /// @param ctx      Shared document context supplying the Scene and selection.
        SceneViewportPanel(Veng::Renderer::Context& context, Veng::AssetManager& assets,
                           Veng::ImGuiLayer& imgui, PrefabEditContext& ctx);
        ~SceneViewportPanel() override;

        [[nodiscard]] Veng::string_view GetTitle() const override { return "Scene Viewport"; }
        [[nodiscard]] Veng::UI::WindowFlags GetWindowFlags() const override
        {
            return Veng::UI::WindowFlags::NoScrollbar | Veng::UI::WindowFlags::NoScrollWithMouse;
        }

        /// @brief Records this frame's scene render so the output is ready for UI::Image.
        void OnRender(Veng::Renderer::CommandBuffer& cmd) override;
        void OnImGui() override;

    private:
        /// @brief Recomputes the camera view from the orbit yaw/pitch/distance about the target.
        void UpdateCamera();

        Veng::Renderer::Context& m_Context;
        Veng::AssetManager& m_Assets;
        Veng::ImGuiLayer& m_ImGui;
        PrefabEditContext& m_Ctx;

        Veng::Unique<Veng::Renderer::SceneRenderer> m_SceneRenderer;
        Veng::CameraView m_Camera;

        /// @brief Orbit camera state about m_Target.
        Veng::f32 m_Yaw = 0.6f;
        Veng::f32 m_Pitch = 0.3f;
        Veng::f32 m_Distance = 4.0f;
        Veng::vec3 m_Target{0.0f};

        Veng::Ref<Veng::Renderer::Sampler> m_SceneSampler;
        Veng::Ref<Veng::ImGuiTexture> m_SceneTexture;

        /// @brief Renderer settings driven by the debug-view dropdown; applied in OnRender.
        Veng::Renderer::SceneRendererSettings m_Settings;
        bool m_SettingsDirty = false;

        Veng::uvec2 m_RenderExtent{};
        Veng::uvec2 m_PendingExtent{};
    };
}
