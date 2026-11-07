#pragma once

#include <array>

#include <Veng/Veng.h>
#include <Veng/Asset/ParticleEffect.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/RenderLayer.h>

#include "Passes/SpriteScenePass.h"

namespace Veng
{
    class Scene;
    class CameraView;
}

namespace Veng::Renderer
{
    /// @brief The most particle effect instances one renderer tracks at once.
    inline constexpr u32 MaxParticleInstances = 1024;

    /// @brief The most emitter records one frame uploads.
    inline constexpr u32 MaxParticleEmittersPerFrame = 256;

    /// @brief The most spawn ranges one frame issues.
    inline constexpr u32 MaxParticleSpawnRanges = 4096;

    /// @brief The most per-frame flipbook extents one frame uploads.
    inline constexpr u32 MaxParticleFrameScalesPerFrame = 16384;

    /// @brief The most fed spawns one frame issues.
    inline constexpr u32 MaxParticleFedSpawnsPerFrame = 16384;

    /// @brief GpuParticleSpawnRange::InstanceEmitter's flag for a range of fed spawns, whose
    ///        FirstIndex is its first record in the frame's fed-spawn table.
    inline constexpr u32 ParticleRangeFed = 0x80000000u;

    /// @brief GpuParticleEmitter::Mode.w bits, mirrored by particle_types.slang.
    namespace ParticleFlag
    {
        /// @brief The billboard's flipbook is six-way.
        inline constexpr u32 SixWay = 1u << 0;
        /// @brief The shift of the two-bit frame blend (the FlipbookFrameBlend integer).
        inline constexpr u32 BlendShift = 1;
        /// @brief The stored temperature is under the sqrt transfer.
        inline constexpr u32 SqrtTemperature = 1u << 3;
        /// @brief The temperature is red of its own texture rather than LightB's alpha.
        inline constexpr u32 TemperatureRed = 1u << 4;
        /// @brief The emitter's emission ramp has keys.
        inline constexpr u32 Emits = 1u << 5;
        /// @brief The flipbook loops.
        inline constexpr u32 Loop = 1u << 6;
        /// @brief Playback starts at a random frame.
        inline constexpr u32 RandomStart = 1u << 7;
    }

    /// @brief GpuParticleInstance::Generation's bit marking an instance drawn nowhere this frame.
    inline constexpr u32 ParticleInstanceHidden = 0x80000000u;

    /// @brief GpuParticleEmitter::Counts.w for a flipbook recording no per-frame extents.
    inline constexpr u32 ParticleNoFrameScales = 0xFFFFFFFFu;

    /// @brief One particle's spawn state (std430, 48 bytes); mirrors particle_types.slang.
    struct GpuParticle
    {
        /// @brief The offset from the instance's anchor at spawn.
        vec3 Offset{0.0f};
        /// @brief Seconds of the instance's playback it spawned at.
        f32 SpawnTime = 0.0f;
        /// @brief The velocity at spawn.
        vec3 Velocity{0.0f};
        /// @brief Seconds it lives.
        f32 Lifetime = 0.0f;
        /// @brief Instance slot in the low 16 bits, its generation in the high 16.
        u32 Instance = 0;
        /// @brief The effect-relative record of the emitter's flipbook it plays.
        u32 Emitter = 0;
        /// @brief The particle's key.
        u32 Random = 0;
        /// @brief Roll rate, radians per second.
        f32 Spin = 0.0f;
    };
    static_assert(sizeof(GpuParticle) == 48, "GpuParticle must match particle_types.slang");

    /// @brief One playing instance (std430, 64 bytes); mirrors particle_types.slang.
    struct GpuParticleInstance
    {
        /// @brief The anchor relative to the eye, along render-space axes.
        vec3 AnchorEye{0.0f};
        /// @brief Seconds of playback.
        f32 Time = 0.0f;
        /// @brief The effect's frame, a quaternion stored (x, y, z, w).
        vec4 Orientation{0.0f, 0.0f, 0.0f, 1.0f};
        /// @brief Added to every initial velocity.
        vec3 InheritVelocity{0.0f};
        /// @brief The instance's seed.
        u32 Seed = 0;
        /// @brief The slot's generation in the low 16 bits, and ParticleInstanceHidden.
        u32 Generation = 0;
        /// @brief The effect's first emitter in the frame's emitter table.
        u32 EmitterBase = 0;
        /// @brief The effect's emitter count.
        u32 EmitterCount = 0;
        /// @brief Multiplies every length the effect authors.
        f32 Scale = 1.0f;
    };
    static_assert(sizeof(GpuParticleInstance) == 64,
                  "GpuParticleInstance must match particle_types.slang");

    /// @brief One emitter of an effect drawn this frame (std430, 1408 bytes); mirrors
    ///        particle_types.slang.
    struct GpuParticleEmitter
    {
        /// @brief x the ParticleRender, y 1 when additive, z the ParticleShape, w ParticleFlag bits.
        uvec4 Mode{0u};
        /// @brief Bindless indices: x the colour sheet or lightA, y lightB, z motion, w temperature.
        uvec4 Textures{0u};
        /// @brief x columns, y rows, z frame count, w the sampler's bindless index.
        uvec4 Grid{1u, 1u, 1u, 0u};
        /// @brief x burst count, y rate total, z the FlipbookAlpha, w the frame-scale base.
        uvec4 Counts{0u, 0u, 0u, ParticleNoFrameScales};
        /// @brief x how many flipbooks the emitter picks from, y the effect-relative index of the
        ///        record for its second (the rest follow it in order); read from the emitter's own
        ///        record, which plays its first.
        uvec4 Variants{1u, 0u, 0u, 0u};
        /// @brief x burst time, y rate start, z rate per second.
        vec4 Spawn{0.0f};
        /// @brief xy the lifetime range, zw the size range.
        vec4 LifetimeSize{0.0f};
        /// @brief xy the speed range, z the speed-by-radius exponent, w the shape radius.
        vec4 Speed{0.0f};
        /// @brief xyz the cone's unit axis in the effect's frame, w the cosine of its half-angle.
        vec4 Cone{0.0f, 1.0f, 0.0f, -1.0f};
        /// @brief x noise amplitude, y noise frequency, z soft distance (below 0 half the width),
        ///        w emission intensity.
        vec4 Noise{0.0f, 1.0f, 0.0f, 1.0f};
        /// @brief xyz acceleration, w drag.
        vec4 Accel{0.0f};
        /// @brief xy the initial roll range, zw the spin range, in radians.
        vec4 Rotation{0.0f};
        /// @brief x frames per second (0 stretches over the life), y motion scale, w aspect.
        vec4 Playback{0.0f};
        /// @brief xy the anchor fraction, zw one tile's extent in UV.
        vec4 Frame{0.5f, 0.5f, 1.0f, 1.0f};
        /// @brief xy half a texel in UV.
        vec4 Inset{0.0f};
        /// @brief rgb a streak's colour, w the seconds of velocity its length spans.
        vec4 Streak{1.0f, 1.0f, 1.0f, 0.05f};
        /// @brief The emission ramp in the sprite pass's form.
        GpuEmissionRamp Ramp;
        /// @brief The over-life tables, ParticleEffect::GetEmitterLut's layout.
        std::array<vec4, 2 * ParticleLutSamples> Lut{};
    };
    static_assert(sizeof(GpuParticleEmitter) == 1424,
                  "GpuParticleEmitter must match particle_types.slang");

    /// @brief One run of particle indices to spawn (std430, 16 bytes); mirrors particle_types.slang.
    /// @brief One fed spawn issued this frame (std430, 32 bytes); mirrors particle_types.slang.
    struct GpuParticleFedSpawn
    {
        /// @brief Where it spawns, as an offset from the instance's anchor.
        vec3 Offset{0.0f};
        /// @brief Seconds of the instance's playback it spawns at.
        f32 Time = 0.0f;
        /// @brief Its fed velocity at spawn.
        vec3 Velocity{0.0f};
        /// @brief Its place in the feed's sequence: the particle's index.
        u32 Index = 0;
    };
    static_assert(sizeof(GpuParticleFedSpawn) == 32,
                  "GpuParticleFedSpawn must match particle_types.slang");

    struct GpuParticleSpawnRange
    {
        /// @brief Instance slot in the low 16 bits, the effect-local emitter index in bits 16-30,
        ///        and ParticleRangeFed for a range of fed spawns.
        u32 InstanceEmitter = 0;
        /// @brief The first particle index, or for a fed range its first fed-spawn record.
        u32 FirstIndex = 0;
        /// @brief How many indices.
        u32 Count = 0;
        /// @brief The spawn threads every earlier range of the frame takes.
        u32 Prefix = 0;
    };
    static_assert(sizeof(GpuParticleSpawnRange) == 16,
                  "GpuParticleSpawnRange must match particle_types.slang");

    /// @brief One live particle as the draw reads it (std430, 112 bytes); mirrors
    ///        particle_types.slang.
    ///
    /// Written by the update pass alone; the CPU only sizes the buffer by it.
    struct GpuParticleDraw
    {
        /// @brief xyz the eye-relative centre (a streak's head), w the width.
        vec4 CentreWidth{0.0f};
        /// @brief xyz a streak's tail relative to its head, w the quad's height.
        vec4 TailHeight{0.0f};
        /// @brief Halves: rgb tint times intensity, a opacity.
        uvec2 TintOpacity{0u};
        /// @brief Screen-plane roll, radians.
        f32 Rotation = 0.0f;
        /// @brief The emitter-table index.
        u32 Emitter = 0;
        /// @brief Frame N plus the fraction toward N + 1.
        f32 FramePosition = 0.0f;
        /// @brief Added to the decoded six-way temperature.
        f32 TemperatureOffset = 0.0f;
        /// @brief Frame N's extent as a fraction of the full frame.
        f32 FrameScale = 1.0f;
        /// @brief Frame N + 1's.
        f32 FrameScaleNext = 1.0f;
        /// @brief The light gather's halves, first quarter.
        uvec4 Light0{0u};
        /// @brief The light gather's halves, second quarter.
        uvec4 Light1{0u};
        /// @brief The light gather's halves, the rest.
        uvec3 Light2{0u};
        /// @brief World depth the particle fades over into opaque geometry; 0 off.
        f32 SoftDistance = 0.0f;
    };
    static_assert(sizeof(GpuParticleDraw) == 112,
                  "GpuParticleDraw must match particle_types.slang");

    /// @brief What one frame's gather hands the particle chain, and what it dropped.
    struct ParticleFramePlan
    {
        /// @brief The instance table, every slot up to the highest in use.
        vector<GpuParticleInstance> Instances;
        /// @brief The emitter records of every effect an instance drawn this frame plays: each
        ///        effect's emitters in order, then the records for their further flipbooks.
        vector<GpuParticleEmitter> Emitters;
        /// @brief This frame's spawn ranges, in prefix order.
        vector<GpuParticleSpawnRange> Ranges;
        /// @brief Per-frame flipbook extents the emitters index.
        vector<f32> FrameScales;
        /// @brief This frame's fed spawns, which its fed ranges index.
        vector<GpuParticleFedSpawn> FedSpawns;
        /// @brief The particles this frame spawns: the sum of the ranges' counts.
        u32 SpawnTotal = 0;
        /// @brief An upper bound on the particles alive after this frame's spawns.
        u32 AliveBound = 0;
        /// @brief An upper bound on the alpha particles drawn this frame.
        u32 AlphaBound = 0;
        /// @brief The spawns this frame issues for emitters that may draw alpha: those whose
        ///        records, or any of their further flipbooks' records, are alpha.
        u32 AlphaSpawnTotal = 0;
        /// @brief How many instances the table holds.
        u32 InstanceCount = 0;
        /// @brief Spawns trimmed past the capacity or the range table, never issued.
        u32 SpawnsDropped = 0;
        /// @brief Instances past the table, not tracked this frame.
        u32 InstancesDropped = 0;
        /// @brief Instances whose emitters did not fit the emitter table, not drawn this frame.
        u32 EmittersDropped = 0;
        /// @brief Instances whose effect waits on a flipbook still loading, not drawn this frame.
        u32 InstancesWaiting = 0;
        /// @brief Whether a drawn emitter fades softly, and so reads the opaque-depth copy.
        bool ReadsSceneDepth = false;

        /// @brief Returns whether the plan tracks no instance at all.
        [[nodiscard]] bool IsEmpty() const { return InstanceCount == 0; }
    };

    /// @brief Builds the record an emitter draws one of its flipbooks with, resolving its shading.
    ///
    /// The record's Variants is left at one flipbook; the gather fills it in on the emitter's first
    /// record once it has laid out the rest.
    /// @param effect        The effect.
    /// @param emitter       The emitter's index in the effect.
    /// @param variant       Which of the emitter's flipbooks; 0 for a streak.
    /// @param frameScales   The frame's extent table, which a flipbook with per-frame extents is
    ///                      appended to (when it fits MaxParticleFrameScalesPerFrame).
    /// @return The record, or nullopt when a billboard's flipbook or its textures are not resident.
    [[nodiscard]] VE_API optional<GpuParticleEmitter>
    BuildParticleEmitter(const ParticleEffect& effect, u32 emitter, u32 variant,
                         vector<f32>& frameScales);

    /// @brief The renderer's view of the scene's particle effect instances, frame to frame.
    ///
    /// Keyed by (entity, playback): a key seen for the first time takes a free slot and bumps the
    /// slot's generation, and a key gone from the scene frees its slot and bumps it again, so its
    /// particles die at the next update by generation mismatch. Each slot keeps its emitters' spawn
    /// cursors — the particles it has issued — so every renderer issues every index exactly once
    /// whatever its Execute cadence, and a renderer meeting an instance late issues only the
    /// particles that can still be alive, each at its exact spawn time. The table resets when the
    /// renderer is pointed at a different scene. Device-free: the gather reads the scene and the
    /// assets, and writes plain records.
    class VE_API ParticleGather
    {
    public:
        /// @brief Gathers a frame: refreshes the table and fills @p plan.
        ///
        /// An instance is tracked while its effect is resident, it is anchored and not finished.
        /// Each tracked instance whose emitters fit the emitter table issues the indices that
        /// spawned since the last frame and can still be alive, as ranges; trimmed past
        /// @p capacity and MaxParticleSpawnRanges and counted as dropped, never deferred.
        ///
        /// An instance on a layer @p visibleLayers omits, or on @p exclude, is tracked and issued
        /// exactly as a drawn one but marked ParticleInstanceHidden: its particles spawn and live
        /// in the pool undrawn, so a view mask hiding it neither resets its emission nor leaves a
        /// backlog to issue when it shows again.
        /// @param scene          The scene drawn.
        /// @param eye            The camera's position in render space.
        /// @param capacity       The renderer's particle capacity.
        /// @param plan           The plan to fill; cleared first.
        /// @param visibleLayers  The view's render-layer mask.
        /// @param exclude        An entity the view does not draw, or Entity::Null.
        void Gather(const Scene& scene, const dvec3& eye, u32 capacity, ParticleFramePlan& plan,
                    u32 visibleLayers = AllRenderLayers, Entity exclude = Entity::Null);

        /// @brief Forgets every instance and cursor.
        void Reset();

    private:
        /// @brief One tracked instance.
        struct Slot
        {
            /// @brief The instance's entity.
            Entity Owner = Entity::Null;
            /// @brief The instance's playback counter.
            u32 Playback = 0;
            /// @brief Bumped on every claim and free.
            u16 Generation = 0;
            /// @brief Whether a key holds the slot.
            bool Used = false;
            /// @brief Whether the key was seen this frame.
            bool Seen = false;
            /// @brief Per emitter, the indices of each stream issued so far.
            vector<ParticleStreamCounts> Cursors;
            /// @brief The feed sequence number every spawn before has been issued or passed.
            u64 FedCursor = 0;
        };

        /// @brief Every slot, the free ones included.
        vector<Slot> m_Slots;
        /// @brief The scene the table describes.
        u64 m_SceneSerial = 0;
    };
}
