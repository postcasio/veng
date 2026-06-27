#pragma once

#include <Veng/Veng.h>
#include <Veng/Math/Ray.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>

#include <utility>

namespace Veng
{
    class Scene;

    namespace Renderer
    {
        class DebugDraw;
    }
}

namespace VengEditor
{
    /// @brief Which transform a manipulation gizmo edits.
    enum class GizmoMode : Veng::u8
    {
        /// @brief Translate along an axis or in a plane.
        Translate,
        /// @brief Rotate about an axis (a ring in that axis's plane).
        Rotate,
        /// @brief Scale along an axis (or uniformly through the center handle).
        Scale,
    };

    /// @brief Which handle of the active gizmo a ray hit, or None.
    ///
    /// Encodes both the kind (axis / plane / uniform) and the axis (X/Y/Z) in one value
    /// so the hit-test, the highlight, and the drag-solve switch on a single enum.
    enum class GizmoHandle : Veng::u8
    {
        /// @brief No handle under the cursor.
        None,
        /// @brief Translate or scale along the X axis; rotate about X.
        AxisX,
        /// @brief Translate or scale along the Y axis; rotate about Y.
        AxisY,
        /// @brief Translate or scale along the Z axis; rotate about Z.
        AxisZ,
        /// @brief Translate in the YZ plane (perpendicular to X).
        PlaneX,
        /// @brief Translate in the XZ plane (perpendicular to Y).
        PlaneY,
        /// @brief Translate in the XY plane (perpendicular to Z).
        PlaneZ,
        /// @brief Uniform scale through the center box.
        Uniform,
    };

    /// @brief Hand-rolled world-space translate / rotate / scale gizmo on the active entity.
    ///
    /// Drawn through a viewport's per-viewport Veng::Renderer::DebugDraw channel (axes for
    /// translate, rings for rotate, axis boxes for scale) and interacted with by analytic
    /// ray-vs-handle hit-testing over Viewport::ScreenToWorldRay — no id buffer, no ImGuizmo.
    /// The gizmo is placed at the active entity's world position and sized in world units scaled
    /// by camera distance, so it stays a roughly constant on-screen size. Manipulation is world
    /// space and pivots on the active entity; a multi-entity selection moves only its Active.
    ///
    /// The panel owns one and drives it: it sets the mode, calls Hover each frame to highlight,
    /// begins a drag on a press over a handle, updates the live Transform each drag frame, and on
    /// release fires the commit seam. The gizmo holds no Scene or entity ownership — every call
    /// takes the Scene and the entity, and the gizmo only retains the in-progress drag state.
    class EditorGizmo
    {
    public:
        /// @brief Returns the current manipulation mode.
        [[nodiscard]] GizmoMode GetMode() const { return m_Mode; }

        /// @brief Sets the manipulation mode (Translate / Rotate / Scale).
        ///
        /// Ignored while a drag is in progress, so a mode key pressed mid-drag does not
        /// switch the handle being dragged out from under the solve.
        /// @param mode  The mode to switch to.
        void SetMode(GizmoMode mode);

        /// @brief Returns true while a handle drag is in progress.
        [[nodiscard]] bool IsDragging() const { return m_Dragging; }

        /// @brief Draws the gizmo for @p entity into the debug-draw accumulator.
        ///
        /// A no-op when the entity is null or not alive. The hovered or active handle is
        /// drawn highlighted. Pushes lines (and the camera position is used only for sizing).
        /// @param debug          The viewport's debug-draw accumulator to push into.
        /// @param scene          The scene the entity lives in.
        /// @param entity         The active entity the gizmo manipulates.
        /// @param cameraPosition The camera world position, for distance-based handle sizing.
        void Draw(Veng::Renderer::DebugDraw& debug, const Veng::Scene& scene, Veng::Entity entity,
                  Veng::vec3 cameraPosition) const;

        /// @brief Hit-tests @p ray against the handles and records the hovered one.
        ///
        /// Updates the hovered handle used by Draw for the highlight; a no-op visual on a miss.
        /// Skipped while dragging (the active handle stays the dragged one). Returns whether a
        /// handle is under the cursor — the panel's "a press here is a manipulation, not a
        /// click-select" test.
        /// @param scene          The scene the entity lives in.
        /// @param entity         The active entity the gizmo manipulates.
        /// @param ray            The cursor world ray (Viewport::ScreenToWorldRay).
        /// @param cameraPosition The camera world position, for distance-based handle sizing.
        /// @return true when a handle is under the cursor.
        [[nodiscard]] bool Hover(const Veng::Scene& scene, Veng::Entity entity,
                                 const Veng::Ray& ray, Veng::vec3 cameraPosition);

        /// @brief Begins a drag if @p ray hits a handle; returns whether a drag started.
        ///
        /// Records the grabbed handle, the entity's start Transform, and the grab anchor the
        /// per-mode solve measures the drag delta from. The whole drag is one logical edit
        /// committed on release.
        /// @param scene          The scene the entity lives in.
        /// @param entity         The active entity the gizmo manipulates.
        /// @param ray            The cursor world ray at the press.
        /// @param cameraPosition The camera world position, for distance-based handle sizing.
        /// @return true when a handle was grabbed and a drag began.
        bool BeginDrag(const Veng::Scene& scene, Veng::Entity entity, const Veng::Ray& ray,
                       Veng::vec3 cameraPosition);

        /// @brief Solves the new Transform for the current drag ray and applies it live.
        ///
        /// Writes the result back through the scene Transform accessor (so the spatial-version
        /// bump fires). A no-op when no drag is in progress or the entity is no longer alive.
        /// @param scene  The scene the entity lives in.
        /// @param entity The active entity the gizmo manipulates.
        /// @param ray    The cursor world ray this frame.
        void Drag(Veng::Scene& scene, Veng::Entity entity, const Veng::Ray& ray);

        /// @brief Ends the drag and returns the start/final Transform pair for the commit.
        ///
        /// The Transform is already applied live each Drag, so the returned pair is the seam an
        /// undo command spans: the Transform before the drag and the live (final) Transform read
        /// back from the entity. Returns nullopt when no drag was in progress.
        /// @param scene  The scene the entity lives in.
        /// @param entity The active entity the gizmo manipulated.
        /// @return The { start, final } Transform pair, or nullopt when not dragging.
        [[nodiscard]] Veng::optional<std::pair<Veng::Transform, Veng::Transform>>
        EndDrag(const Veng::Scene& scene, Veng::Entity entity);

    private:
        /// @brief World-space placement of the gizmo: the pivot and its three world axes.
        struct Placement
        {
            /// @brief The gizmo pivot — the active entity's world position.
            Veng::vec3 Origin{0.0f};
            /// @brief World-space X/Y/Z axis directions (the entity's parent frame is ignored: world space).
            Veng::vec3 Axes[3]{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
            /// @brief Handle scale in world units, eased with camera distance for constant on-screen size.
            Veng::f32 Scale = 1.0f;
        };

        /// @brief Computes the gizmo placement for an entity (origin from its world matrix, world axes, sized by camera distance).
        [[nodiscard]] static Placement
        ComputePlacement(const Veng::Scene& scene, Veng::Entity entity, Veng::vec3 cameraPosition);

        /// @brief Hit-tests a ray against the handles for the current mode at @p placement.
        ///
        /// @param placement  The gizmo placement to test against.
        /// @param ray        The cursor world ray.
        /// @return The hit handle, or GizmoHandle::None on a miss.
        [[nodiscard]] GizmoHandle Pick(const Placement& placement, const Veng::Ray& ray) const;

        /// @brief The current manipulation mode.
        GizmoMode m_Mode = GizmoMode::Translate;

        /// @brief The handle highlighted this frame (hovered, or the dragged one while dragging).
        GizmoHandle m_Hovered = GizmoHandle::None;

        /// @brief True while a handle drag is in progress.
        bool m_Dragging = false;

        /// @brief The handle being dragged; meaningful only while m_Dragging.
        GizmoHandle m_DragHandle = GizmoHandle::None;

        /// @brief The active entity's Transform at the drag start (the commit's "before").
        Veng::Transform m_StartTransform;

        /// @brief The gizmo placement captured at the drag start (the drag solves against it).
        Placement m_DragPlacement;

        /// @brief The grab anchor the per-mode solve measures the drag delta from.
        ///
        /// For an axis/plane translate it is the world point the press ray hit on the
        /// constraint; for a rotate it is the initial angle in the ring plane; for a scale it
        /// is the initial signed axis projection. Interpreted per mode by Drag.
        Veng::vec3 m_GrabAnchor{0.0f};

        /// @brief The active entity's start world position (the rotate/scale pivot).
        Veng::vec3 m_StartWorldPosition{0.0f};
    };
}
