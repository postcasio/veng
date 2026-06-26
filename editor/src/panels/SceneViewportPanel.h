#pragma once

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Scene/Camera.h>

#include <VengEditor/EditorPanel.h>

#include "EditorCamera.h"
#include "panels/PrefabEditContext.h"

namespace Veng
{
    class Application;
    class AssetManager;
    class ImGuiLayer;
    class ImGuiTexture;
    class Input;
    class InputRouter;
    class Texture;
    struct LevelRenderSettings;

    namespace Renderer
    {
        class Context;
        class Sampler;
    }
}

namespace VengEditor
{
    class PrefabEditorPanel;

    /// @brief Scene viewport child of a prefab editor: shows the document's Scene from
    /// an Unreal-style editor camera in a UI::Image, with a translucent toolbar overlay.
    ///
    /// Owns a registered Offscreen Veng::Renderer::Viewport sized to the panel's content
    /// region and an EditorCamera the user drives (RMB fly + WASDQE, MMB pan, Alt orbit,
    /// wheel dolly, F to frame); reads the live Scene from the shared PrefabEditContext.
    /// The engine drive-list renders the viewport each frame from the region and view the
    /// panel pushes in OnUI; the panel records no scene render itself. The toolbar overlay
    /// drives the document's play session and the camera/debug controls.
    class SceneViewportPanel final : public EditorPanel
    {
    public:
        /// @brief Constructs the viewport over the document's edit context.
        /// @param app       Application the owned viewport registers into the drive-list.
        /// @param assets    Asset manager the renderer resolves materials through.
        /// @param imgui     ImGui layer the render target is registered with.
        /// @param ctx       Shared document context supplying the Scene and selection.
        /// @param input     Frame-coherent input service the editor camera reads.
        /// @param router    Input router whose gameplay focus captures the mouse during Play.
        /// @param document  Owning document the toolbar drives play state through.
        SceneViewportPanel(Veng::Application& app, Veng::AssetManager& assets,
                           Veng::ImGuiLayer& imgui, PrefabEditContext& ctx, Veng::Input& input,
                           Veng::InputRouter& router, PrefabEditorPanel& document);
        ~SceneViewportPanel() override;

        [[nodiscard]] Veng::string_view GetTitle() const override { return "Scene Viewport"; }
        [[nodiscard]] Veng::UI::WindowFlags GetWindowFlags() const override
        {
            return Veng::UI::WindowFlags::NoScrollbar | Veng::UI::WindowFlags::NoScrollWithMouse;
        }

        void OnUI() override;

        /// @brief Applies a level's render subset to the viewport, mirroring the runtime mapping.
        ///
        /// Folds the battery toggles (Bloom/Shadows/AO) into the SceneRendererSettings —
        /// flagged for a Configure only when one actually changed, so a per-edit call never
        /// forces a needless recompile — and stores the per-frame Exposure / BloomIntensity the
        /// pushed ViewState carries each frame. The level editor pushes its live settings here so
        /// an edit shows in the viewport immediately, ahead of the recook.
        /// @param render  The level's render settings.
        void ApplyLevelRenderSettings(const Veng::LevelRenderSettings& render);

    private:
        /// @brief Draws the toolbar overlay (play/camera/debug controls) over the viewport image.
        void DrawToolbar();

        /// @brief Draws the centered banner telling the player how to release a captured mouse.
        void DrawCaptureNotice();

        /// @brief Frames the camera on the selected entities, or the whole scene when none are selected.
        void FrameSelection();

        /// @brief Walks the scene and pushes a debug-draw billboard + wireframe gizmo per Light/Camera.
        ///
        /// Pushes into the viewport's DebugDraw accumulator (consumed by the DebugDrawScenePass next
        /// render): an icon billboard at each Light/Camera's world position, a light's range sphere
        /// (point) or spot cone (spot), and a camera's frustum. A no-op when the icon handles failed
        /// to resolve (no icon pack mounted). The DebugDraw battery toggle is forced on while gizmos
        /// are pushed. Click-to-select is out of scope; Viewport::ScreenToWorldRay is the seam a
        /// later billboard-picking follow-on extends.
        void PushGizmos();

        Veng::AssetManager& m_Assets;
        Veng::ImGuiLayer& m_ImGui;
        PrefabEditContext& m_Ctx;
        Veng::Input& m_Input;
        Veng::InputRouter& m_Router;
        PrefabEditorPanel& m_Document;

        /// @brief The owned Offscreen viewport; registered into the app's drive-list on construction.
        Veng::Unique<Veng::Renderer::Viewport> m_Viewport;

        /// @brief The user-driven editor camera.
        EditorCamera m_Camera;

        /// @brief The view the camera produced last OnUI, pushed as the viewport's ViewState.
        Veng::CameraView m_View;

        Veng::Ref<Veng::Renderer::Sampler> m_SceneSampler;
        Veng::Ref<Veng::ImGuiTexture> m_SceneTexture;

        /// @brief Renderer settings driven by the debug-view dropdown; pushed to Configure when dirty.
        Veng::Renderer::SceneRendererSettings m_Settings;
        bool m_SettingsDirty = false;

        /// @brief Per-frame tonemap exposure written into the pushed ViewState each frame.
        Veng::f32 m_Exposure = 1.0f;
        /// @brief Per-frame bloom composite intensity written into the pushed ViewState each frame.
        Veng::f32 m_BloomIntensity = 1.0f;

        /// @brief Last extent the ImGui texture was fetched at; re-fetch when the viewport resizes.
        Veng::uvec2 m_TextureExtent{};

        /// @brief Resident light-icon texture (keeps its bindless TextureHandle alive); null if unmounted.
        Veng::AssetHandle<Veng::Texture> m_LightIcon;
        /// @brief Resident camera-icon texture (keeps its bindless TextureHandle alive); null if unmounted.
        Veng::AssetHandle<Veng::Texture> m_CameraIcon;
    };
}
