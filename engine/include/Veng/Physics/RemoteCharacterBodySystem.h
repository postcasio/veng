#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    class Scene;

    /// @brief Builtin Sim system giving a remote character a kinematic collision body to be blocked by.
    ///
    /// A character this peer does not simulate — another player's, mirrored as Tier::Remote and
    /// displayed by the RemoteInterpolationSystem rather than advanced — carries no capsule: the
    /// CharacterMovementSystem skips it and the orphan sweep releases it. But the *local* character's
    /// capsule must still collide with it, and two CharacterVirtual capsules do not see each other.
    /// So this system keeps a **kinematic** RigidBody + capsule Collider on every remote character,
    /// synced to its interpolated Transform, exactly matching the character's own capsule shape. The
    /// local character slides against that body and is blocked by it; the body is kinematic, so
    /// neither character pushes the other — they simply block. Remote characters are never simulated
    /// locally (that would mean predicting another peer's input), only interpolated and made solid.
    ///
    /// The proxy is a purely local decoration: the components it adds are not replicated and are
    /// added and removed against the peer's own authority. A character this peer *does* simulate — a
    /// server's authoritative character, or a client's own predicted one — is given a capsule by the
    /// mover instead and has its proxy removed here, so a possession change that flips a character
    /// local drops the proxy and one that flips it remote grows one. On the server, where every
    /// character is authoritative, the system is a no-op.
    ///
    /// A no-op when the scene owns no PhysicsWorld. Runs before PhysicsSystem, so the proxy body is
    /// reconciled and driven to the interpolated pose in the same tick it is added.
    class VE_API RemoteCharacterBodySystem final : public SceneSystem
    {
    public:
        /// @brief Grows or drops each character's kinematic collision proxy against this peer's authority.
        /// @param scene    The scene whose characters are proxied.
        /// @param delta    Time in seconds since the previous tick (unused).
        /// @param context  Per-tick services; carries this peer's authority.
        void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override;
    };
}

VE_SYSTEM(::Veng::RemoteCharacterBodySystem, 0xA5459D9013864D14ULL, "Remote Character Body");
