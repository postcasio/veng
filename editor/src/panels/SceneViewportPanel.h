#pragma once

#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Scene/Camera.h>

#include <VengEditor/EditorPanel.h>

#include "EditorCamera.h"
#include "panels/PrefabEditContext.h"

namespace Veng
{
    class AssetManager;
    class ImGuiLayer;
    class ImGuiTexture;
    class Input;

    namespace Renderer
    {
        class Context;
        class CommandBuffer;
        class Sampler;
    }
}

namespace VengEditor
{
    class PrefabEditorPanel;

    /// @brief Scene viewport child of a prefab editor: renders the document's Scene from
    /// an Unreal-style editor camera into a UI::Image, with a translucent toolbar overlay.
    ///
    /// Owns a SceneRenderer sized to the panel's content region (debounced resize) and an
    /// EditorCamera the user drives (RMB fly + WASDQE, MMB pan, Alt orbit, wheel dolly, F to
    /// frame); reads the live Scene from the shared PrefabEditContext. The toolbar overlay
    /// drives the document's play session and the camera/debug controls. The host records
    /// the scene render via OnRender before the ImGui frame is built, so the output is
    /// sampleable when OnImGui draws it.
    class SceneViewportPanel final : public EditorPanel
    {
    public:
        /// @brief Constructs the viewport over the document's edit context.
        /// @param context   Render context the SceneRenderer is created against.
        /// @param assets    Asset manager the renderer resolves materials through.
        /// @param imgui     ImGui layer the render target is registered with.
        /// @param ctx       Shared document context supplying the Scene and selection.
        /// @param input     Frame-coherent input service the editor camera reads.
        /// @param document  Owning document the toolbar drives play state through.
        SceneViewportPanel(Veng::Renderer::Context& context, Veng::AssetManager& assets,
                           Veng::ImGuiLayer& imgui, PrefabEditContext& ctx, Veng::Input& input,
                           PrefabEditorPanel& document);
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
        /// @brief Draws the toolbar overlay (play/camera/debug controls) over the viewport image.
        void DrawToolbar();

        /// @brief Frames the camera on the selected entities, or the whole scene when none are selected.
        void FrameSelection();

        Veng::Renderer::Context& m_Context;
        Veng::AssetManager& m_Assets;
        Veng::ImGuiLayer& m_ImGui;
        PrefabEditContext& m_Ctx;
        Veng::Input& m_Input;
        PrefabEditorPanel& m_Document;

        Veng::Unique<Veng::Renderer::SceneRenderer> m_SceneRenderer;

        /// @brief The user-driven editor camera.
        EditorCamera m_Camera;

        /// @brief The view the camera produced last OnImGui, consumed by OnRender (one-frame latency).
        Veng::CameraView m_View;

        Veng::Ref<Veng::Renderer::Sampler> m_SceneSampler;
        Veng::Ref<Veng::ImGuiTexture> m_SceneTexture;

        /// @brief Renderer settings driven by the debug-view dropdown; applied in OnRender.
        Veng::Renderer::SceneRendererSettings m_Settings;
        bool m_SettingsDirty = false;

        Veng::uvec2 m_RenderExtent{};
        Veng::uvec2 m_PendingExtent{};
    };
}
