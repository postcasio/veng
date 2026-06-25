#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    /// @brief Sim-phase system that turns published root-motion deltas into pawn movement.
    ///
    /// For each entity with (Transform, RootMotionDelta), it rotates the model-local delta an
    /// Animator in Drive mode published into the entity's orientation, scales it by the entity's
    /// scale, and adds it to the entity's position — making a clip's baked locomotion drive
    /// authoritative movement. The delta is consumed (zeroed) on apply, so a pawn whose Animator
    /// stops publishing does not keep drifting. Because the producing AnimationSystem runs in the
    /// View phase after Sim, the delta consumed here is one tick old; this is the deterministic,
    /// replicable application point a net layer predicts.
    class RootMotionDriveSystem final : public SceneSystem
    {
    public:
        /// @brief Applies and clears each entity's RootMotionDelta into its Transform.
        /// @param scene    The scene whose pawns are moved.
        /// @param delta    Time since the previous tick (unused — the value is a displacement, not a rate).
        /// @param context  Per-tick services (unused).
        void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override;
    };

    VE_SYSTEM(RootMotionDriveSystem, 0x4FAA68CAB0EB4DB7ULL, "Root Motion Drive");
}
