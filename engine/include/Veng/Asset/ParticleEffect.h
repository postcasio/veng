#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/Flipbook.h>

namespace Veng
{
    /// @brief How a particle emitter's particles are drawn.
    ///
    /// Stored as the underlying integer in CookedParticleEmitter::Render.
    enum class ParticleRender : u32
    {
        /// @brief A camera-facing quad playing a flipbook.
        Billboard = 0,
        /// @brief A quad stretched along the particle's velocity, in one HDR colour: a spark.
        Streak = 1,
    };

    /// @brief How a particle emitter's particles composite into the scene.
    ///
    /// Stored as the underlying integer in CookedParticleEmitter::Blend.
    enum class ParticleBlend : u32
    {
        /// @brief The flipbook's own recommendation; a billboard-only choice.
        Asset = 0,
        /// @brief Composited over the scene by coverage, sorted back to front.
        Alpha = 1,
        /// @brief Added onto the scene, in no particular order.
        Additive = 2,
    };

    /// @brief Where a particle emitter places its particles at spawn.
    ///
    /// Stored as the underlying integer in CookedParticleEmitter::Shape.
    enum class ParticleShape : u32
    {
        /// @brief Every particle at the effect's origin.
        Point = 0,
        /// @brief Uniformly through a ball of the shape's radius.
        Sphere = 1,
        /// @brief On the surface of a sphere of the shape's radius.
        Shell = 2,
    };

    /// @brief How a billboard particle plays its flipbook.
    ///
    /// Stored as the underlying integer in CookedParticleEmitter::PlaybackMode.
    enum class ParticlePlayback : u32
    {
        /// @brief The whole flipbook is stretched over the particle's lifetime.
        Lifetime = 0,
        /// @brief The flipbook plays at a fixed rate, wrapping when it loops and holding its last
        ///        frame when it does not.
        Fps = 1,
    };

    /// @brief The most emitters one particle effect holds.
    inline constexpr u32 MaxParticleEmitters = 16;

    /// @brief The most light flashes one particle effect holds.
    inline constexpr u32 MaxParticleFlashes = 4;

    /// @brief The most keys one over-life curve holds.
    inline constexpr u32 MaxParticleCurveKeys = 16;

    /// @brief How many samples each over-life curve is baked into on load.
    inline constexpr u32 ParticleLutSamples = 32;

    /// @brief One key of a scalar curve over normalised age.
    struct ParticleCurveKey
    {
        /// @brief The normalised age the key sits at, in [0, 1].
        f32 T = 0.0f;
        /// @brief The curve's value there.
        f32 Value = 0.0f;
    };

    /// @brief One key of a colour curve over normalised age.
    struct ParticleColorKey
    {
        /// @brief The normalised age the key sits at, in [0, 1].
        f32 T = 0.0f;
        /// @brief The linear colour there.
        vec3 Value{1.0f};
    };

    /// @brief One layer of a particle effect: what it spawns, how the particles move, and how they
    ///        draw.
    ///
    /// Every [min, max] range (x the minimum, y the maximum) is drawn uniformly per particle.
    /// Lengths are world units at an instance Scale of 1 and times are seconds. The schema document
    /// `PARTICLE_EFFECT_SCHEMA.md` in the cooker gives each field's source spelling and validation.
    struct ParticleEmitterDesc
    {
        /// @brief A label for diagnostics.
        string Name;
        /// @brief How the particles draw.
        ParticleRender Render = ParticleRender::Billboard;
        /// @brief The flipbook a billboard plays; unused by a streak.
        AssetHandle<Flipbook> Flipbook;
        /// @brief How the particles composite.
        ParticleBlend Blend = ParticleBlend::Asset;
        /// @brief Particles spawned at once at BurstTime; 0 for no burst.
        u32 BurstCount = 0;
        /// @brief Seconds into the effect the burst spawns.
        f32 BurstTime = 0.0f;
        /// @brief Particles spawned per second from RateStart; 0 for no rate.
        f32 RatePerSecond = 0.0f;
        /// @brief Seconds into the effect the rate starts.
        f32 RateStart = 0.0f;
        /// @brief Seconds the rate runs; 0 runs it until the instance stops emitting.
        f32 RateDuration = 0.0f;
        /// @brief Seconds each particle lives.
        vec2 Lifetime{1.0f, 1.0f};
        /// @brief Where particles spawn.
        ParticleShape Shape = ParticleShape::Point;
        /// @brief The spawn shape's radius R.
        f32 ShapeRadius = 0.0f;
        /// @brief The radial speed, before the radius factor and the noise.
        vec2 Speed{0.0f, 0.0f};
        /// @brief The axis spawn directions gather about, in the effect's own frame.
        vec3 ConeAxis{0.0f, 1.0f, 0.0f};
        /// @brief The cone's half-angle in degrees, in (0, 180]; 180 is the whole sphere.
        f32 ConeAngle = 180.0f;
        /// @brief The exponent of (r / R) the speed is scaled by; 1 is the self-similar expansion.
        f32 SpeedByRadius = 0.0f;
        /// @brief The relative amplitude of the speed noise over spawn directions; 0 for none.
        f32 NoiseAmplitude = 0.0f;
        /// @brief The spatial frequency of the speed noise over spawn directions.
        f32 NoiseFrequency = 1.0f;
        /// @brief Constant acceleration in the stable frame, world units per second squared.
        vec3 Acceleration{0.0f};
        /// @brief Linear drag rate k, per second: velocity relaxes as e^(-k t).
        f32 Drag = 0.0f;
        /// @brief The quad's width.
        vec2 Size{1.0f, 1.0f};
        /// @brief The initial screen-plane roll, in degrees.
        vec2 Rotation{0.0f, 0.0f};
        /// @brief The roll rate, in degrees per second.
        vec2 Spin{0.0f, 0.0f};
        /// @brief How a billboard plays its flipbook.
        ParticlePlayback Playback = ParticlePlayback::Lifetime;
        /// @brief The rate for ParticlePlayback::Fps, in frames per second; 0 takes the flipbook's.
        f32 PlaybackFps = 0.0f;
        /// @brief Whether a billboard starts its playback at a random frame.
        bool RandomStart = false;
        /// @brief The soft-edge depth distance; unset takes the default rule (half the quad's
        ///        width for a six-way flipbook, off otherwise). Always off for a streak.
        optional<f32> SoftDistance;
        /// @brief Replaces a six-way flipbook's emission ramp and ignition; unset takes the
        ///        flipbook's.
        optional<FlipbookEmission> Emission;
        /// @brief A streak's linear HDR colour.
        vec3 StreakColor{1.0f};
        /// @brief Seconds of velocity a streak's length spans.
        f32 StreakLengthScale = 0.05f;
        /// @brief The size multiplier over normalised age; empty is constant 1.
        vector<ParticleCurveKey> SizeCurve;
        /// @brief The opacity multiplier over normalised age; empty is constant 1.
        vector<ParticleCurveKey> OpacityCurve;
        /// @brief The RGB tint over normalised age; empty is constant white.
        vector<ParticleColorKey> TintCurve;
        /// @brief The intensity multiplier over normalised age; empty is constant 1.
        vector<ParticleCurveKey> IntensityCurve;
        /// @brief The offset added to a six-way flipbook's temperature over normalised age; empty
        ///        is constant 0.
        vector<ParticleCurveKey> TemperatureCurve;
    };

    /// @brief One light flash of a particle effect: a point light over a window of the effect.
    struct ParticleFlashDesc
    {
        /// @brief Seconds into the effect the flash begins.
        f32 Time = 0.0f;
        /// @brief Seconds the flash lasts.
        f32 Duration = 0.2f;
        /// @brief The light's linear colour.
        vec3 Color{1.0f};
        /// @brief Peak intensity in lumens, as Light::Intensity.
        f32 Intensity = 0.0f;
        /// @brief The light's range in world units, as Light::Range.
        f32 Range = 0.0f;
        /// @brief The light's source radius in world units, as Light::Radius.
        f32 Radius = 0.0f;
        /// @brief The intensity multiplier over the flash's normalised life; empty is constant 1.
        vector<ParticleCurveKey> Curve;
    };

    /// @brief Construction parameters for a ParticleEffect.
    struct ParticleEffectInfo
    {
        /// @brief Debug name.
        string Name;
        /// @brief The emitters, at most MaxParticleEmitters.
        vector<ParticleEmitterDesc> Emitters;
        /// @brief The light flashes, at most MaxParticleFlashes.
        vector<ParticleFlashDesc> Flashes;
    };

    /// @brief Layered particle emitters and light flashes, played as one effect — the particle
    ///        effect asset.
    ///
    /// Cooked from a `*.particles.json` source (the format is documented in the cooker's
    /// `PARTICLE_EFFECT_SCHEMA.md`), or built in code with Create. The asset is data: a
    /// ParticleEffectInstance plays it, the renderer simulates and draws its particles, and it
    /// carries no playback state of its own. Its over-life curves are baked on construction into
    /// ParticleLutSamples-sample tables the renderer copies to the GPU.
    class VE_API ParticleEffect
    {
    public:
        /// @brief Builds an effect from its emitters and flashes.
        /// @param info  The emitters and flashes.
        /// @pre At most MaxParticleEmitters emitters and MaxParticleFlashes flashes; every curve
        ///      holds at most MaxParticleCurveKeys keys with strictly ascending T in [0, 1]; a
        ///      billboard names a flipbook and a streak does not; every range has min <= max.
        /// @return The effect.
        [[nodiscard]] static Ref<ParticleEffect> Create(const ParticleEffectInfo& info);

        /// @brief Returns the debug name.
        [[nodiscard]] const string& GetName() const { return m_Info.Name; }

        /// @brief Returns the emitters.
        [[nodiscard]] const vector<ParticleEmitterDesc>& GetEmitters() const
        {
            return m_Info.Emitters;
        }

        /// @brief Returns the light flashes.
        [[nodiscard]] const vector<ParticleFlashDesc>& GetFlashes() const { return m_Info.Flashes; }

        /// @brief Returns an emitter's baked over-life tables.
        ///
        /// 2 * ParticleLutSamples values: first (size, opacity, intensity, temperature offset),
        /// then (tint r, g, b, 0). Sample i is the curve at normalised age i / (ParticleLutSamples
        /// - 1), so the first and last samples are the curves' end keys.
        /// @param emitter  The emitter's index.
        /// @return The tables, in that order.
        [[nodiscard]] std::span<const vec4> GetEmitterLut(u32 emitter) const;

    private:
        /// @brief One emitter's two tables, back to back.
        using EmitterLut = std::array<vec4, 2 * ParticleLutSamples>;

        /// @brief Constructs from validated parameters; use Create.
        explicit ParticleEffect(ParticleEffectInfo info);

        /// @brief The emitters and flashes.
        ParticleEffectInfo m_Info;
        /// @brief One baked table pair per emitter.
        vector<EmitterLut> m_Luts;
    };

    /// @brief AssetTypeTrait specialization mapping ParticleEffect to AssetTypes::ParticleEffect.
    template <>
    struct AssetTypeTrait<ParticleEffect>
    {
        /// @brief The asset type tag for ParticleEffect.
        static constexpr AssetTypeId Type = AssetTypes::ParticleEffect;
    };

    /// @brief Evaluates a piecewise-linear scalar curve over normalised age.
    ///
    /// Clamped to the end keys outside them; one key is constant.
    /// @param keys      The keys, ascending in T.
    /// @param t         The normalised age.
    /// @param fallback  The value of an empty curve.
    /// @return The curve's value at @p t.
    [[nodiscard]] VE_API f32 EvaluateParticleCurve(std::span<const ParticleCurveKey> keys, f32 t,
                                                   f32 fallback);

    /// @brief Evaluates a piecewise-linear colour curve over normalised age.
    /// @param keys      The keys, ascending in T.
    /// @param t         The normalised age.
    /// @param fallback  The value of an empty curve.
    /// @return The curve's colour at @p t.
    [[nodiscard]] VE_API vec3 EvaluateParticleCurve(std::span<const ParticleColorKey> keys, f32 t,
                                                    vec3 fallback);

    /// @brief An emitter's particles, as two index streams.
    ///
    /// A particle's index is its identity: burst indices are [0, BurstCount) and rate particle j is
    /// index BurstCount + j. The burst spawns at one time and the rate at times rising with j, so
    /// each stream is ordered in time but the two interleave, and every schedule question is
    /// answered per stream.
    struct ParticleStreamCounts
    {
        /// @brief Burst particles: 0 or the emitter's BurstCount.
        u32 Burst = 0;
        /// @brief Rate particles, counted from j = 0.
        u32 Rate = 0;
    };

    /// @brief Returns the time a particle spawns at, in seconds into the effect.
    /// @param emitter  The emitter.
    /// @param index    The particle's index (see ParticleStreamCounts).
    /// @return BurstTime for a burst index, RateStart + j / RatePerSecond for rate particle j.
    [[nodiscard]] VE_API f32 ParticleSpawnTime(const ParticleEmitterDesc& emitter, u32 index);

    /// @brief Returns how many particles of each stream have spawned by a playback time.
    ///
    /// Counts the burst when its time is at most @p t, and every rate particle whose spawn time is
    /// at most @p t, within the emitter's rate duration. A rate particle at or after @p stopTime
    /// never spawns; a negative @p stopTime is an instance still emitting.
    /// @param emitter   The emitter.
    /// @param t         Seconds of playback.
    /// @param stopTime  The time emission stopped, or below 0 while still emitting.
    /// @return The spawned prefix of each stream.
    [[nodiscard]] VE_API ParticleStreamCounts ParticleSpawnedBy(const ParticleEmitterDesc& emitter,
                                                                f32 t, f32 stopTime);

    /// @brief Returns, per stream, the first particle that can still be alive at a playback time.
    ///
    /// Every particle before it spawned at or before t - Lifetime.y and is certainly dead, since no
    /// lifetime exceeds the maximum. The burst's first possibly alive is 0 while it can be alive
    /// and BurstCount once it cannot.
    /// @param emitter  The emitter.
    /// @param t        Seconds of playback.
    /// @return The first possibly-alive index of each stream.
    [[nodiscard]] VE_API ParticleStreamCounts
    ParticleFirstPossiblyAlive(const ParticleEmitterDesc& emitter, f32 t);

    /// @brief Returns the time by which every particle and flash of an effect has ended.
    ///
    /// The latest of each emitter's last spawn plus its Lifetime.y and each flash's end. While a
    /// rate emitter with no duration still emits (@p stopTime below 0) it is infinite. Every
    /// particle the effect can spawn is dead by it, so an instance whose time has reached it has
    /// finished, known on the CPU with no readback.
    /// @param effect    The effect.
    /// @param stopTime  The time emission stopped, or below 0 while still emitting.
    /// @return The end time, in seconds of playback.
    [[nodiscard]] VE_API f32 ParticleEffectEndTime(const ParticleEffect& effect, f32 stopTime);

    /// @brief An upper bound on one emitter's particles alive at once.
    /// @param burstCount     The burst's particle count.
    /// @param ratePerSecond  The rate, per second.
    /// @param lifetimeMax    The longest lifetime.
    /// @return burstCount + ceil(ratePerSecond * lifetimeMax).
    [[nodiscard]] inline u64 ParticleEmitterMaxAlive(const u32 burstCount, const f32 ratePerSecond,
                                                     const f32 lifetimeMax)
    {
        const f64 rate = static_cast<f64>(std::max(ratePerSecond, 0.0f)) *
                         static_cast<f64>(std::max(lifetimeMax, 0.0f));
        return static_cast<u64>(burstCount) + static_cast<u64>(std::ceil(rate));
    }

    /// @brief An upper bound on one instance's particles alive at once.
    ///
    /// The sum over the emitters of ParticleEmitterMaxAlive, for budgeting a consumer's effects
    /// against the renderer's ParticleCapacity.
    /// @param effect  The effect.
    /// @return The bound, saturated at the u32 range.
    [[nodiscard]] VE_API u32 ParticleEffectMaxAlive(const ParticleEffect& effect);
}
