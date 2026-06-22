#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/Camera.h>

namespace VengEditor
{
    /// @brief One frame of viewport input, gathered by the panel from engine Input + UI hover/focus.
    ///
    /// The controller is pure: it pulls in no ImGui, Window, or engine Input header. The
    /// owning panel fills this struct each frame (relative mouse motion, wheel, button and
    /// modifier state, the WASDQE movement keys, and the viewport aspect) and hands it to
    /// EditorCamera::Update, which returns the resulting CameraView.
    struct EditorCameraInput
    {
        /// @brief Pointer is over the viewport image.
        bool Hovered = false;
        /// @brief Viewport has interaction focus.
        bool Focused = false;
        /// @brief Relative mouse motion this frame, in pixels.
        Veng::vec2 MouseDelta{0};
        /// @brief Wheel delta this frame.
        Veng::vec2 ScrollDelta{0};
        /// @brief Left mouse button held.
        bool MouseLeft = false;
        /// @brief Right mouse button held.
        bool MouseRight = false;
        /// @brief Middle mouse button held.
        bool MouseMiddle = false;
        /// @brief Alt modifier held.
        bool Alt = false;
        /// @brief Shift modifier held.
        bool Shift = false;
        /// @brief Forward movement key (W) held.
        bool Forward = false;
        /// @brief Back movement key (S) held.
        bool Back = false;
        /// @brief Strafe-left movement key (A) held.
        bool Left = false;
        /// @brief Strafe-right movement key (D) held.
        bool Right = false;
        /// @brief Up movement key (E) held.
        bool Up = false;
        /// @brief Down movement key (Q) held.
        bool Down = false;
        /// @brief Frame-selection key (F) pressed this frame.
        bool FrameSelection = false;
        /// @brief Viewport width divided by height, for the projection.
        Veng::f32 Aspect = 1.0f;
    };

    /// @brief Unreal-style editor fly/orbit camera producing a CameraView.
    ///
    /// A pure controller: it holds position + yaw/pitch orientation plus an orbit pivot and
    /// distance, advances one frame from an EditorCameraInput, and rebuilds a Veng::CameraView
    /// each Update. The panel owns it, fills the input from the engine Input + UI hover/focus,
    /// and feeds GetView() to the scene renderer. RMB drives fly mode (mouse-look + WASDQE),
    /// LMB dolly+turn, MMB pan, Alt+LMB orbit, and the wheel dolly-zooms (or adjusts fly speed
    /// while flying). Frame() recenters the pivot and distance onto a world-space bounds.
    class EditorCamera
    {
    public:
        /// @brief Constructs the camera at a default pose looking at the origin.
        EditorCamera();

        /// @brief Advances the camera one frame and returns the resulting view.
        ///
        /// Applies the active navigation mode selected by the input's button/modifier state
        /// (only while focused, or hovered for the wheel), rebuilds the internal pose, and
        /// refreshes the CameraView. Fly-mode WASDQE movement scales by the elapsed time.
        /// @param in  This frame's gathered viewport input.
        /// @param dt  Elapsed time this frame, in seconds.
        /// @return true while the camera is actively navigating and the cursor should be
        ///         locked to the viewport for the duration of the drag (RMB fly active).
        ///         This is a transient navigation lock, released the instant the drag ends —
        ///         distinct from the play session's sticky mouse capture.
        bool Update(const EditorCameraInput& in, Veng::f32 dt);

        /// @brief Returns the camera view rebuilt by the last Update.
        [[nodiscard]] Veng::CameraView GetView() const;

        /// @brief Frames a world-space bounds: recenters the pivot and pulls the distance to fit.
        ///
        /// Keeps the current view direction; moves the pivot to the bounds center and sets a
        /// distance that fits the sphere of the given radius in the current field of view, then
        /// recomputes the camera position from pivot, direction, and distance.
        /// @param center  World-space center of the bounds to frame.
        /// @param radius  World-space radius of the bounds to frame.
        void Frame(Veng::vec3 center, Veng::f32 radius);

        /// @brief Returns the fly-mode movement speed, in world units per second.
        [[nodiscard]] Veng::f32 GetFlySpeed() const;
        /// @brief Sets the fly-mode movement speed, in world units per second.
        void SetFlySpeed(Veng::f32 speed);

        /// @brief Returns the vertical field of view, in radians.
        [[nodiscard]] Veng::f32 GetFovY() const;
        /// @brief Sets the vertical field of view, in radians.
        void SetFovY(Veng::f32 fovY);

    private:
        /// @brief Recomputes m_View from the current pose and aspect.
        void RebuildView(Veng::f32 aspect);
        /// @brief Returns the camera forward vector derived from yaw/pitch.
        [[nodiscard]] Veng::vec3 Forward() const;
        /// @brief Returns the camera right vector (forward × world up, normalized).
        [[nodiscard]] Veng::vec3 Right() const;
        /// @brief Returns the camera up vector (right × forward).
        [[nodiscard]] Veng::vec3 Up() const;

        /// @brief World-space camera position.
        Veng::vec3 m_Position{0.0f, 2.0f, 6.0f};
        /// @brief Yaw angle in radians (rotation about world up).
        Veng::f32 m_Yaw = 0.0f;
        /// @brief Pitch angle in radians (rotation about the camera's right axis).
        Veng::f32 m_Pitch = 0.0f;
        /// @brief Orbit pivot — the point Alt+orbit and dolly-zoom revolve around.
        Veng::vec3 m_Pivot{0.0f};
        /// @brief Distance from the camera to the pivot.
        Veng::f32 m_Distance = 6.0f;
        /// @brief Fly-mode movement speed, world units per second.
        Veng::f32 m_FlySpeed = 6.0f;
        /// @brief Vertical field of view, in radians.
        Veng::f32 m_FovY = glm::radians(45.0f);
        /// @brief Near clip distance.
        Veng::f32 m_Near = 0.1f;
        /// @brief Far clip distance.
        Veng::f32 m_Far = 1000.0f;
        /// @brief View/projection rebuilt each Update.
        Veng::CameraView m_View;
    };
}
