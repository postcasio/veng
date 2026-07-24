#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    class Scene;

    /// @brief Builtin Sim system resolving each Interactor's focused Interactable.
    ///
    /// Each tick, for every entity carrying an Interactor, it sweeps an Overlap of the interactor's
    /// Reach against the scene's PhysicsWorld, keeps the candidates that carry an enabled Interactable
    /// and fall within both the interactor's view cone and their own Range, picks the best by angle
    /// then distance, and writes it to the Interactor's Focused (Entity::Null when none qualifies).
    ///
    /// It publishes the resolution and nothing more: it neither draws a prompt nor fires anything. A
    /// prompt reads Focused and the Interactable's Verb; firing is a separate InteractRequest a control
    /// system stamps, drained by whatever system owns that kind of interactable. A no-op when the scene
    /// owns no PhysicsWorld — with no world there are no candidates, so every Focused clears.
    class VE_API InteractionSystem final : public SceneSystem
    {
    public:
        /// @brief Resolves and writes every Interactor's focused Interactable for this tick.
        /// @param scene    The scene whose interactors are resolved.
        /// @param delta    Time in seconds since the previous tick (unused; the resolve is stateless).
        /// @param context  Per-tick services (unused).
        void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override;
    };
}

VE_SYSTEM(::Veng::InteractionSystem, 0x877B655C5306BE04ULL, "Interaction");
