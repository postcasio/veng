#pragma once

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Scene/Camera.h>

#include <VengEditor/EditorPanel.h>

#include "EditorCamera.h"
#include "EditorGizmo.h"
#include "panels/PrefabEditContext.h"

#include <Veng/Scene/Components.h>

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
    class CommandStack;

    /// @brief Scene viewport child of a prefab editor: shows the document's Scene from
    /// an Unreal-style editor camera in a UI::Image, with a translucent toolbar overlay.
    ///
    /// Owns a registered Offscreen Veng::Renderer::Viewport sized to the panel's content
    /// region and an EditorCamera the user drives (RMB fly + WASDQE, MMB pan, Alt orbit,
    /// wheel dolly, F to frame); reads the live Scene from the shared PrefabEditContext.
    /// The engine drive-list renders the viewport each frame from the region and view the
    /// panel pushes in OnUI; the panel records no scene render itself. The toolbar overlay
    /// drives the viewport-local camera and debug-view controls; the play transport and the
    /// gizmo-mode selector live on the document toolbar. The gizmo mode is shared document state
    /// (PrefabEditContext::Gizmo) every viewport reads; this panel owns only the per-viewport
    /// EditorGizmo hover/drag state.
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
        /// @param commands  The document's undo/redo stack a gizmo drag is committed through.
        SceneViewportPanel(Veng::Application& app, Veng::AssetManager& assets,
                           Veng::ImGuiLayer& imgui, PrefabEditContext& ctx, Veng::Input& input,
                           Veng::InputRouter& router, CommandStack& commands);
        ~SceneViewportPanel() override;

        [[nodiscard]] Veng::string_view GetTitle() const override { return "Scene Viewport"; }
        [[nodiscard]] Veng::UI::WindowFlags GetWindowFlags() const override
        {
            return Veng::UI::WindowFlags::NoScrollbar | Veng::UI::WindowFlags::NoScrollWithMouse;
        }

        void OnUI() override;

        /// @brief Returns the owned Offscreen viewport, or null before it is constructed.
        ///
        /// The document's screenshot seam (editor.screenshot_panel) captures this viewport's ready
        /// output through the shared render-thread Download path.
        [[nodiscard]] Veng::Renderer::Viewport* GetViewport() const { return m_Viewport.get(); }

        /// @brief Applies a level's post/pipeline render subset to the viewport, mirroring the runtime mapping.
        ///
        /// Folds the topology toggles (Bloom/Shadows/AO) into the SceneRendererSettings, flagged for
        /// a Configure only when one actually changed, so a per-edit call never forces a needless
        /// recompile, and stores the per-frame Exposure / BloomIntensity the pushed ViewState carries
        /// each frame. The sky/environment is not here — it is resolved from the scene's components
        /// each frame in OnUI (ApplySceneSky). The level editor pushes its live settings here so an
        /// edit shows in the viewport immediately, ahead of the recook.
        /// @param render  The level's post/pipeline render settings.
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
        /// are pushed. Each icon billboard carries its owning entity's pick id (index + 1), so a click
        /// over the icon selects that entity through the renderer's id pass (the same readback meshes
        /// resolve through) — the billboard-picking seam HandleClickToSelect drives.
        void PushGizmos();

        /// @brief Issues a viewport pick for a left-click inside the content rect, routing into selection.
        ///
        /// On a left-click that the camera/play interaction did not consume, hit-tests the cursor
        /// against the viewport and (on a hit) calls Viewport::Pick. The async result updates the
        /// shared selection: a plain click SelectOnly, Ctrl/Cmd-click Toggle, and a background (no
        /// entity) plain click Clears. A guard prevents issuing a second pick while one is in flight,
        /// so a held button does not queue a burst. The callback is guarded by a panel-owned alive
        /// flag (so a resolve landing after the panel is torn down is dropped) on top of the renderer's
        /// scene-epoch + caller-liveness guard (a resolve after a Play/Stop scene swap is dropped).
        /// @param hovered   Whether the viewport image is hovered this frame.
        /// @param consumed  Whether the click was already consumed (camera drag, play capture).
        void HandleClickToSelect(bool hovered, bool consumed);

        /// @brief Drives the gizmo over the active selection: hover / press / drag / release.
        ///
        /// Builds the cursor world ray (Viewport::ScreenToWorldRay) and routes the content-rect
        /// mouse gizmo-first: on a press over a handle it begins a drag; while held it solves the
        /// new Transform live; on release it fires OnCommit. A no-op when nothing is selected or
        /// the click was consumed by the camera / play capture. Returns whether the gizmo owns the
        /// mouse this frame (a press over a handle, or an in-progress drag) so the caller suppresses
        /// the click-to-select fall-through.
        /// @param hovered   Whether the viewport image is hovered this frame.
        /// @param consumed  Whether the click was already consumed (camera drag, play capture).
        /// @return true when the gizmo consumed the press/drag (selection must not also fire).
        bool HandleGizmo(bool hovered, bool consumed);

        /// @brief Whether the cursor is over a gizmo handle of the active selection this frame.
        ///
        /// Hit-tests the cursor ray against the active entity's gizmo (Viewport::ScreenToWorldRay
        /// + EditorGizmo::Hover) using the prior frame's view. Used to deny camera drag-ownership to
        /// a press that is grabbing a handle, so manipulating a gizmo does not also move the camera.
        /// False while playing, with no scene, or with no live active entity.
        /// @return true when a handle is under the cursor.
        bool CursorOverGizmoHandle();

        /// @brief Records a finished gizmo drag as the edit spanning @p start → @p final.
        ///
        /// The Transform is already applied live during the drag; this is the seam an undo
        /// command spans the whole drag through. It applies the final Transform (a no-op past the
        /// already-applied value) so the edit is durable at the commit boundary.
        /// @param start  The active entity's Transform before the drag.
        /// @param final  The active entity's Transform after the drag.
        void OnCommit(const Veng::Transform& start, const Veng::Transform& final);

        Veng::AssetManager& m_Assets;
        Veng::ImGuiLayer& m_ImGui;
        PrefabEditContext& m_Ctx;
        Veng::Input& m_Input;
        Veng::InputRouter& m_Router;
        CommandStack& m_Commands;

        /// @brief The owned Offscreen viewport; registered into the app's drive-list on construction.
        Veng::Unique<Veng::Renderer::Viewport> m_Viewport;

        /// @brief The user-driven editor camera.
        EditorCamera m_Camera;

        /// @brief The translate/rotate/scale gizmo on the active selection.
        EditorGizmo m_Gizmo;

        /// @brief The view the camera produced last OnUI, pushed as the viewport's ViewState.
        Veng::CameraView m_View;

        /// @brief True while a camera mouse-drag owns the current button hold.
        ///
        /// Latched on a press over the viewport image that is not grabbing a gizmo handle, held
        /// until every mouse button releases. Only while owned do the camera's mouse-drag modes
        /// (LMB dolly, Alt-orbit, MMB pan, RMB fly) run — so a drag onto a dock tab/toolbar or a
        /// press on a gizmo handle never moves the camera.
        bool m_CameraDragOwned = false;

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

        /// @brief True between issuing a pick and its callback firing — the one-pick-in-flight guard.
        ///
        /// A held mouse button re-enters HandleClickToSelect each frame; this prevents queuing a
        /// burst of picks before the first resolves (a frame or two later through the readback).
        bool m_PickInFlight = false;

        /// @brief Panel-alive flag the pick callback captures by value to drop a post-teardown resolve.
        ///
        /// Set false in the destructor before the viewport is dropped. The renderer's pending pick
        /// lives in the owned viewport (dropped here), so a resolve cannot fire after teardown — but
        /// the flag makes the callback's own liveness explicit rather than relying on that ordering.
        Veng::Ref<bool> m_Alive;
    };
}
