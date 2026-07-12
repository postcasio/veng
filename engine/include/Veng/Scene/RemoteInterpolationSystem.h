#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/SceneSystem.h>

#include <span>

namespace Veng
{
    class Scene;

    /// @brief One buffered pose sample for a remote entity, keyed by the server tick it represents.
    ///
    /// The interpolation buffer holds a short, tick-ordered run of these; the remote-interpolation
    /// system blends the two bracketing a render time that lags the newest received tick. Local
    /// TRS mirrors Transform's fields (the wire replicates the local pose).
    struct RemoteSample
    {
        /// @brief The server tick this sample's state was snapshotted at.
        u64 ServerTick = 0;
        /// @brief Local position in parent space at that tick.
        vec3 Position{0.0f};
        /// @brief Local rotation in parent space at that tick.
        quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
        /// @brief Local scale in parent space at that tick.
        vec3 Scale{1.0f};
    };

    /// @brief Client-side ring of a remote entity's replicated pose samples, ordered by server tick.
    ///
    /// Added by the replication layer to every Remote-tier entity: each Transform snapshot appends a
    /// sample here rather than writing the live Transform, and the View-phase RemoteInterpolationSystem
    /// reads the ring to write the displayed Transform interpolated in the past. Runtime-only — carries
    /// no reflected field, so it never serializes and never rides the wire (it is client display state,
    /// derived from the snapshots that do).
    struct RemoteInterpolation
    {
        /// @brief Tick-ordered pose samples; the newest is last. Bounded to a short window.
        vector<RemoteSample> Samples;
    };

    /// @brief A decaying render-space pose offset applied to a predicted entity at gather time.
    ///
    /// When a client's prediction is corrected — its predicted pose is restored to the authoritative
    /// state and replayed forward — the visible pose would jump. To hide the discontinuity, the
    /// pre-correction visible pose is held as this offset from the corrected sim pose and decayed to
    /// zero over ~100–200 ms. It is applied only where transforms feed the render gather (never
    /// written back into Transform, which stays authoritative sim state), so the mesh eases into the
    /// corrected pose while the simulation is already there. Runtime-only: it carries no reflected
    /// field, so it never serializes and never rides the wire. A negligible offset removes itself.
    struct PredictionError
    {
        /// @brief World-space position offset added to the render pose; decays to zero.
        vec3 Position{0.0f};
        /// @brief World-space rotation offset applied to the render pose about the entity origin; decays to identity.
        quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
    };

    /// @brief Decays a render residual toward zero, frame-rate-independent (ExpApproach per component).
    ///
    /// Steps @p error's position and rotation toward identity by the ExpApproach fraction over @p
    /// delta at @p speed (1/seconds), so the ease is identical at any frame rate. Used by the
    /// View-phase decay of PredictionError.
    /// @param error  The residual to step (position offset + rotation offset).
    /// @param delta  The elapsed frame time, in seconds.
    /// @param speed  The decay rate, in 1/seconds.
    /// @return The stepped residual.
    [[nodiscard]] VE_API PredictionError DecayPredictionError(const PredictionError& error,
                                                              f32 delta, f32 speed);

    /// @brief True when a render residual is small enough to drop (below a fixed position/rotation threshold).
    /// @param error  The residual to test.
    /// @return True when the offset is negligible and the component can be removed.
    [[nodiscard]] VE_API bool IsPredictionErrorNegligible(const PredictionError& error);

    /// @brief Blends the two samples bracketing @p renderTick into a displayed local pose.
    ///
    /// The pure core of remote interpolation: over a tick-ordered sample run, returns the sample TRS
    /// at @p renderTick, linearly (position/scale) and spherically (rotation) interpolated between the
    /// bracketing samples. The buffer under/overrun policy is a hold: a @p renderTick at or before the
    /// oldest sample returns the oldest, at or after the newest returns the newest — so a stalled or
    /// still-filling buffer shows the nearest real pose rather than extrapolating. Device-free and
    /// deterministic, so it is the unit-tested core the system drives.
    /// @param samples     The tick-ordered sample run (ascending ServerTick); empty yields nullopt.
    /// @param renderTick  The (fractional) server tick to resolve the pose at.
    /// @return The blended local pose, or nullopt when @p samples is empty.
    [[nodiscard]] VE_API optional<Transform>
    SampleRemoteInterpolation(std::span<const RemoteSample> samples, f64 renderTick);

    /// @brief View-phase system that writes each remote entity's displayed Transform from its sample buffer.
    ///
    /// For every entity with (Transform, RemoteInterpolation) — the client's Remote-tier mirrors — it
    /// advances a playback clock (in server-tick units, by the frame delta and the configured sim tick
    /// rate) that lags the newest received sample by InterpolationDelay, resolves each buffer at that
    /// render tick through SampleRemoteInterpolation, and writes the result as the entity's Transform.
    /// It then drops samples fully behind the render tick, bounding the buffer. Because it runs in the
    /// View phase the produced pose is presentation only — never authoritative, never on the wire. With
    /// no remote entities the system idles, so the single-player render path is untouched.
    class RemoteInterpolationSystem final : public SceneSystem
    {
    public:
        /// @brief Client-side interpolation knobs (Plan 07 threads these from ApplicationInfo).
        struct Settings
        {
            /// @brief Server ticks between snapshots — the sample spacing the delay is counted in.
            u64 SnapshotInterval = 2;
            /// @brief How many snapshot intervals in the past to render, for smoothness under one lost snapshot.
            u64 InterpolationDelayIntervals = 2;
            /// @brief The simulation tick rate the playback clock advances real frame time in.
            f64 SimTickRate = 60.0;
        };

        /// @brief Returns Phase::View — remote poses are display state derived after the Sim phase.
        [[nodiscard]] Phase GetPhase() const override { return Phase::View; }

        /// @brief Replaces the interpolation settings.
        /// @param settings  The new snapshot-interval, delay, and tick-rate knobs.
        void SetSettings(const Settings& settings) { m_Settings = settings; }

        /// @brief Returns the current interpolation settings.
        [[nodiscard]] const Settings& GetSettings() const { return m_Settings; }

        /// @brief Advances the playback clock and writes each remote entity's interpolated Transform.
        /// @param scene    The client scene whose remote mirrors are updated.
        /// @param delta    Time in seconds since the previous frame.
        /// @param context  Per-tick services (unused).
        void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override;

    private:
        /// @brief Interpolation knobs; defaults match the server's snapshot cadence.
        Settings m_Settings;
        /// @brief Playback position in (fractional) server-tick units; lags the newest received sample.
        f64 m_PlaybackTick = 0.0;
        /// @brief False until the first frame with samples seeds the playback clock.
        bool m_Initialized = false;
    };
}

VE_TYPE(::Veng::RemoteInterpolation, 0x4DB6B8EC5D0385AAULL);

VE_TYPE(::Veng::PredictionError, 0x6594EF08023B7948ULL);

VE_SYSTEM(::Veng::RemoteInterpolationSystem, 0x3E37AEC6CF468687ULL, "Remote Interpolation");
