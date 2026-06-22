#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    struct Transform;
    struct CameraFollow;

    /// @brief Computes the camera Transform that trails a target by a follow relationship.
    ///
    /// The camera is placed at the target's world position plus the follow Offset rotated
    /// into the target's world orientation, and oriented to look at the target. When the
    /// follow's Damping is positive, the result is exponentially smoothed from the camera's
    /// current Transform toward that goal over delta; a zero Damping snaps to the goal. Pure
    /// math — no scene, no device — so it is the deterministic core both the camera-rig
    /// system and the unit tests drive.
    /// @param current     The camera's current Transform (the smoothing start point).
    /// @param targetWorld The target entity's world matrix (from WorldMatrix in Transforms.h).
    /// @param follow      The follow Offset and Damping.
    /// @param delta       Time in seconds since the previous tick.
    /// @return The camera Transform for this tick.
    [[nodiscard]] Transform FollowCamera(const Transform& current, const mat4& targetWorld,
                                         const CameraFollow& follow, f32 delta);

    /// @brief View-phase system that trails each follow camera behind its target.
    ///
    /// Runs in the View phase, so it reads pawn state the Sim phase finalized this tick.
    /// For every entity with (Transform, CameraFollow) whose Target is a live entity with a
    /// Transform, it writes the camera entity's Transform through FollowCamera, through the
    /// scene accessor so the spatial-version bookkeeping is correct. The produced camera pose
    /// is purely local — never authoritative, never on the wire.
    class CameraRigSystem final : public SceneSystem
    {
    public:
        /// @brief Returns Phase::View — the rig derives view state after the Sim phase.
        [[nodiscard]] Phase GetPhase() const override { return Phase::View; }

        /// @brief Trails each (Transform, CameraFollow) camera behind its Target.
        /// @param scene    The scene whose follow cameras are updated.
        /// @param delta    Time in seconds since the previous tick.
        /// @param context  Per-tick services (unused).
        void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override;
    };
}
