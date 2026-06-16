#pragma once

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Scene.h>

#include <VengEditor/EditorPanel.h>

namespace Veng
{
    class AssetManager;
    class ImGuiTexture;
    class ImGuiLayer;
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
    // The scene viewport: owns a SceneRenderer driving the hello-triangle prefab
    // scene, renders it each frame, and shows GetOutput() in an ImGui::Image. The
    // image resizes to the panel's content region (debounced — the SceneRenderer
    // is recreated only when the size settles to a new non-zero value).
    //
    // It records the scene render itself, so the host hands it the frame's command
    // buffer before drawing the panel.
    class SceneViewportPanel final : public EditorPanel
    {
    public:
        SceneViewportPanel(Veng::Renderer::Context& context, Veng::AssetManager& assets,
                           Veng::ImGuiLayer& imgui, Veng::TypeRegistry& types);
        ~SceneViewportPanel() override;

        [[nodiscard]] Veng::string_view Title() const override { return "Scene Viewport"; }

        // Record this frame's scene render. Called by the host before OnImGui so
        // the output is ready for the ImGui::Image sample.
        void Render(Veng::Renderer::CommandBuffer& cmd);

        void OnImGui() override;

    private:
        void BuildScene();
        void RegisterOutput();

        Veng::Renderer::Context& m_Context;
        Veng::AssetManager& m_Assets;
        Veng::ImGuiLayer& m_ImGui;
        Veng::TypeRegistry& m_Types;

        Veng::Unique<Veng::Renderer::SceneRenderer> m_SceneRenderer;
        Veng::Unique<Veng::Scene> m_Scene;
        Veng::Camera m_Camera;

        Veng::AssetHandle<Veng::Material> m_BrickMaterial;

        Veng::Ref<Veng::Renderer::Sampler> m_Sampler;
        Veng::Ref<Veng::ImGuiTexture> m_Texture;

        Veng::uvec2 m_RenderExtent{};
        Veng::uvec2 m_PendingExtent{};
        Veng::f32 m_TimeAccum = 0.0f;
    };
}
