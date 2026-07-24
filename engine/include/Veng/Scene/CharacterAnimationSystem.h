#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    class Scene;

    /// @brief The state name the character-animation drive requests while a character is airborne.
    ///
    /// A game authors an AnimationState of this name (a jump/fall clip) for the drive to select while
    /// the character is off the ground; a set with no such state keeps playing the blend, so the name
    /// is a convention rather than a requirement.
    inline constexpr const char* CharacterAirborneState = "Airborne";

    /// @brief Builtin View-phase system mapping a character's motion state onto its animation drive.
    ///
    /// For every entity carrying a CharacterState, it writes the character's ground-plane speed into
    /// an AnimationBlend::Parameter (driving a speed blend such as idle/walk/jog/sprint) and requests
    /// the airborne state on an AnimationStateSet while the character is off the ground. It is a
    /// separate system from AnimationSystem and its input side only: it writes the two fields the
    /// AnimationSystem reads, so a consumer that drives the blend from something other than a
    /// character controller simply does not name this system, and the animation plays from whatever
    /// wrote the fields.
    ///
    /// Runs in the View phase before AnimationSystem, so within one tick the character's finalized Sim
    /// state feeds the blend the same tick it is posed. An entity with a CharacterState but no
    /// AnimationBlend or AnimationStateSet is skipped — the mapping is opt-in per field.
    class CharacterAnimationSystem final : public SceneSystem
    {
    public:
        /// @brief Returns Phase::View — it derives animation input from finalized Sim state.
        [[nodiscard]] Phase GetPhase() const override { return Phase::View; }

        /// @brief Writes each character's speed and air state onto its animation drive fields.
        /// @param scene    The scene whose characters are read.
        /// @param delta    Time in seconds since the previous tick (unused).
        /// @param context  Per-tick services (unused).
        void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override;
    };
}

VE_SYSTEM(::Veng::CharacterAnimationSystem, 0x7F54CFA48FFDA144ULL, "Character Animation");
