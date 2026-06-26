#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    struct Transform;
    struct ConstantMotion;

    /// @brief Integrates one tick of a ConstantMotion's velocities into a Transform.
    ///
    /// Advances the position by the linear velocity and the rotation by the angular
    /// velocity (an axis-angle vector — magnitude is radians/sec), both scaled by delta and
    /// applied in the motion's MotionSpace: Local rotates the velocities into the transform's
    /// current orientation and post-multiplies the spin; World applies them in the parent
    /// frame and pre-multiplies the spin. A zero velocity leaves that channel unchanged. Pure
    /// math — no scene, no device — so it is the deterministic core the system and the unit
    /// tests both drive.
    /// @param transform  The transform mutated in place.
    /// @param motion     The constant linear and angular velocity to integrate.
    /// @param delta      Time in seconds since the previous tick.
    void IntegrateConstantMotion(Transform& transform, const ConstantMotion& motion, f32 delta);

    /// @brief Generic gameplay system drifting and spinning each ConstantMotion entity.
    ///
    /// Per entity with (Transform, ConstantMotion), applies IntegrateConstantMotion, writing
    /// through the scene Transform accessor so the spatial-version bookkeeping is correct. The
    /// motion is autonomous — driven by authored data, not by input or an Intent — so it is
    /// deterministic and Sim-phase by default. Ships with the engine; selected per level
    /// through the SystemRegistry like any other gameplay system.
    class ConstantMotionSystem final : public SceneSystem
    {
    public:
        /// @brief Integrates every (Transform, ConstantMotion) entity's velocities.
        /// @param scene    The scene whose entities are moved.
        /// @param delta    Time in seconds since the previous tick.
        /// @param context  Per-tick services (unused).
        void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override;
    };

    VE_SYSTEM(ConstantMotionSystem, 0x98C368063567AB92ULL, "Constant Motion");
}
