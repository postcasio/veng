#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    struct Transform;
    struct Intent;
    struct Mover;

    /// @brief Integrates one tick of an Intent into a Transform, scaled by a Mover.
    ///
    /// Translates the transform by the Intent's pawn-local move, rotated into the
    /// transform's current orientation, times the Mover's MoveSpeed and delta; rotates
    /// it by the Intent's look delta (yaw about local up, pitch about local right) times
    /// the Mover's TurnSpeed. A zero Intent leaves the transform unchanged. Pure math —
    /// no scene, no device — so it is the deterministic core both the movement system and
    /// the unit tests drive.
    /// @param transform  The pawn transform mutated in place.
    /// @param intent     The desired move and look this tick.
    /// @param mover      The per-pawn speed tuning.
    /// @param delta      Time in seconds since the previous tick.
    void IntegrateMovement(Transform& transform, const Intent& intent, const Mover& mover,
                           f32 delta);

    /// @brief Generic gameplay system integrating each pawn's Intent into its Transform.
    ///
    /// The authoritative movement step: per entity with (Transform, Intent), it applies
    /// IntegrateMovement scaled by the entity's Mover (or a default Mover when absent),
    /// writing through the scene Transform accessor so the spatial-version bookkeeping is
    /// correct. It reads intent and pawn state only — never device input — so the same
    /// movement drives a player, an AI, or a remote producer that wrote the Intent. Ships
    /// with the engine; the game-specific Intent producer (the control mapping) lives in
    /// the game.
    class MovementSystem final : public SceneSystem
    {
    public:
        /// @brief Integrates every (Transform, Intent) pawn's Intent into its Transform.
        /// @param scene    The scene whose pawns are moved.
        /// @param delta    Time in seconds since the previous tick.
        /// @param context  Per-tick services (unused).
        void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override;
    };
}
