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
    class TypeRegistry;
    class Material;

    namespace Renderer
    {
        class Context;
        class CommandBuffer;
        class ImGuiCompositePass;
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

        [[nodiscard]] Veng::string_view GetTitle() const override { return "Scene Viewport"; }
        [[nodiscard]] ImGuiWindowFlags GetWindowFlags() const override
        {
            return ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        }

        // Record this frame's scene render. Called by the host before OnImGui so
        // the output is ready for the ImGui::Image sample.
        void Render(Veng::Renderer::CommandBuffer& cmd);

        // The scene the viewport renders, read by the inspector as a const Scene*.
        [[nodiscard]] Veng::Scene& GetScene() const { return *m_Scene; }

        // The prefab's spawned root entity — the inspector's default selection,
        // the sphere carrying Name/Transform/MeshRenderer/Spinner.
        [[nodiscard]] Veng::optional<Veng::Entity> PrimaryEntity() const { return m_PrimaryEntity; }

        void OnImGui() override;

    private:
        void BuildScene();

        Veng::Renderer::Context& m_Context;
        Veng::AssetManager& m_Assets;
        Veng::ImGuiLayer& m_ImGui;
        Veng::TypeRegistry& m_Types;

        Veng::Unique<Veng::Renderer::SceneRenderer> m_SceneRenderer;
        Veng::Unique<Veng::Scene> m_Scene;
        Veng::Camera m_Camera;

        Veng::AssetHandle<Veng::Material> m_BrickMaterial;

        // Owns the ImGui scene texture and the pre-Render sampleability barrier
        // (panel-only mode — the scene goes inside this panel via ImGui::Image).
        Veng::Unique<Veng::Renderer::ImGuiCompositePass> m_Composite;

        Veng::optional<Veng::Entity> m_PrimaryEntity;

        Veng::uvec2 m_RenderExtent{};
        Veng::uvec2 m_PendingExtent{};

        // Per-entity accumulated spin angle (keyed by entity index), advanced each
        // frame by the entity's Spinner speed.
        Veng::map<Veng::u32, Veng::f32> m_SpinAccum;
    };
}
