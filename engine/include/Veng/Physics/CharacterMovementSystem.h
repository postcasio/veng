#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    class Scene;

    /// @brief Builtin Sim system driving each CharacterController capsule from its Intent.
    ///
    /// The character analogue of MovementSystem, and its alternative rather than its companion: it
    /// sits in the same catalogue slot, so a level names one or the other in its `systems` array —
    /// a pawn that flies names MovementSystem, a pawn that walks names this — and neither knows
    /// about the other. It reads Intent's local-frame move vector and action bits (never raw device
    /// state), so a player, an AI, or a remote producer that wrote the Intent drives the character
    /// the same way.
    ///
    /// Each tick it reconciles the scene's CharacterController components against the world's
    /// capsules (creating, re-creating and destroying them so the world matches the components),
    /// resolves each capsule's up from the gravity field at its position — the same evaluator a
    /// dynamic body integrates against, so a kinematic character and a dynamic crate in one volume
    /// agree by construction — advances the capsule against the world, and writes the result into
    /// a CharacterState the rest of the game reads. A character reached by no gravity source keeps
    /// its last up and floats, and a large field discontinuity that survives the field's own
    /// blending is slewed into a smooth reorientation rather than a snap.
    ///
    /// A no-op when the scene owns no PhysicsWorld. Like the other authoritative advancers it
    /// touches only the characters this peer simulates (see HasAuthority), so a client's Sim phase
    /// never fights the snapshot stream for a remote character.
    ///
    /// A level names this **before** PhysicsSystem, so the capsules move against the previous tick's
    /// finalized world exactly as the flying mover writes its Transform before the solver steps.
    class VE_API CharacterMovementSystem final : public SceneSystem
    {
    public:
        /// @brief Reconciles, resolves each up from gravity, and advances every character capsule.
        /// @param scene    The scene whose characters are driven.
        /// @param delta    Time in seconds since the previous tick.
        /// @param context  Per-tick services; carries this peer's authority.
        void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override;

        /// @brief Destroys every character capsule the scene's world holds when play ends.
        /// @param scene    The scene whose capsules are released.
        /// @param context  Per-tick services (unused).
        void OnStop(Scene& scene, const SystemContext& context) override;
    };
}

VE_SYSTEM(::Veng::CharacterMovementSystem, 0x5D419CD2327E567BULL, "Character Movement");
