#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    class Scene;
    class ParticleEffect;
    struct ParticleEffectInstance;
    struct Light;

    /// @brief Anchors a particle effect instance at its entity's current world pose.
    ///
    /// Sets Anchor to the entity's world position plus the scene's FloatingOrigin position (so the
    /// anchor is in the stable frame, or world space for a scene without one), Orientation to its
    /// world rotation, the emitter offsets to zero at the current Time, Anchored to true, and —
    /// when Seed is 0 — derives a seed from the entity and the playback counter.
    /// @param scene     The scene the entity lives in.
    /// @param entity    The instance's entity.
    /// @param instance  The instance to anchor.
    VE_API void LatchParticleEffect(const Scene& scene, Entity entity,
                                    ParticleEffectInstance& instance);

    /// @brief Starts an instance's playback over from the beginning.
    ///
    /// Resets Time, StopTime, Finished and Anchored, and advances Playback, so every renderer
    /// treats the restart as a new instance and the next update latches it afresh.
    /// @param instance  The instance to restart.
    VE_API void RestartParticleEffect(ParticleEffectInstance& instance);

    /// @brief The light an effect's flashes make at a playback time.
    ///
    /// The colour is the intensity-weighted mean of the live flashes' colours, the intensity the
    /// sum of each flash's lumens times its curve, and the range and radius the largest among them
    /// times @p scale. A flash is live over [Time, Time + Duration).
    /// @param effect  The effect.
    /// @param time    Seconds of playback.
    /// @param scale   The instance's Scale, applied to the range and radius.
    /// @return The point light, or nullopt while no flash is live.
    [[nodiscard]] VE_API optional<Light> ParticleFlashLight(const ParticleEffect& effect, f32 time,
                                                            f32 scale);

    /// @brief View-phase system that plays every ParticleEffectInstance.
    ///
    /// Per instance whose effect is resident it latches an unanchored instance, records StopTime
    /// the first update after Emitting goes false, advances Time by the frame delta, and sets
    /// Finished once Time reaches ParticleEffectEndTime — the CPU's exact knowledge that every
    /// particle has died, so a scene simulated with no renderer still retires its effects. A
    /// WorldSpace instance's Emitter is moved to the entity's offset from the anchor at the new
    /// Time, the old one kept as PreviousEmitter at PreviousTime, which is the span the renderer
    /// interpolates each particle's spawn point across. An
    /// instance whose effect is still loading holds at its start, as a sprite does.
    ///
    /// An effect with light flashes owns its entity's Light: while a flash is live the system
    /// writes a point Light from ParticleFlashLight, and removes it once none is, overwriting any
    /// Light the entity carried beside such an effect.
    ///
    /// It runs ahead of FlipbookSystem, so Finished is current when the scene's EffectPool retires
    /// finished effects. It does nothing in a scene with no instances; a level lists it to play
    /// particle effects. Presentation only: it writes nothing authoritative.
    class VE_API ParticleEffectSystem final : public SceneSystem
    {
    public:
        /// @brief Returns Phase::View — playback is presentation, advanced once per frame.
        [[nodiscard]] Phase GetPhase() const override { return Phase::View; }

        /// @brief Advances every instance and drives its flashes.
        /// @param scene    The scene whose effects play.
        /// @param delta    Time in seconds since the previous frame.
        /// @param context  Per-tick services (unused).
        void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override;
    };
}

VE_SYSTEM(::Veng::ParticleEffectSystem, 0x7610A07079059B52ULL, "ParticleEffect");
