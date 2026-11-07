#include "ParticleGather.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

#include <Veng/Asset/Flipbook.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include "ParticleMath.h"

namespace Veng::Renderer
{
    namespace
    {
        // A rate emitter with no duration, in GpuParticleEmitter::Counts.y.
        constexpr u32 UnboundedRate = std::numeric_limits<u32>::max();

        u64 KeyOf(const Entity entity, const u32 playback)
        {
            const u64 bits = (static_cast<u64>(entity.Generation) << 32) | entity.Index;
            return bits * 0x9E3779B97F4A7C15ULL ^ playback;
        }

        bool IsAdditive(const ParticleEmitterDesc& emitter, const Flipbook* flipbook)
        {
            switch (emitter.Blend)
            {
            case ParticleBlend::Alpha:
                return false;
            case ParticleBlend::Additive:
                return true;
            case ParticleBlend::Asset:
                break;
            }
            return flipbook != nullptr && flipbook->GetBlend() == FlipbookBlend::Additive;
        }

        u32 CountsBetween(const u32 cursor, const u32 first)
        {
            return cursor > first ? cursor - first : 0u;
        }

        // The flipbooks an emitter's particles pick from; one record for a streak.
        u32 VariantCount(const ParticleEmitterDesc& emitter)
        {
            return std::max(static_cast<u32>(emitter.Flipbooks.size()), 1u);
        }
    }

    optional<GpuParticleEmitter> BuildParticleEmitter(const ParticleEffect& effect,
                                                      const u32 emitterIndex, const u32 variant,
                                                      vector<f32>& frameScales)
    {
        const ParticleEmitterDesc& emitter = effect.GetEmitters()[emitterIndex];
        const ParticleSpawnParams spawn = MakeParticleSpawnParams(emitter);
        const bool billboard = emitter.Render == ParticleRender::Billboard;
        const Flipbook* flipbook = billboard && variant < emitter.Flipbooks.size()
                                       ? emitter.Flipbooks[variant].Get()
                                       : nullptr;
        if (billboard && (flipbook == nullptr || flipbook->GetAtlas() == nullptr))
        {
            return std::nullopt;
        }
        const bool sixWay =
            flipbook != nullptr && flipbook->GetShading() == FlipbookShading::SixWay;
        if (sixWay &&
            (flipbook->GetLightB() == nullptr || flipbook->GetTemperatureTexture() == nullptr))
        {
            return std::nullopt;
        }

        GpuParticleEmitter record;
        u32 flags = 0;
        if (flipbook != nullptr)
        {
            FlipbookFrameBlend blend = flipbook->GetFrameBlend();
            if (blend == FlipbookFrameBlend::Motion && flipbook->GetMotion() == nullptr)
            {
                blend = FlipbookFrameBlend::Crossfade;
            }
            flags |= static_cast<u32>(blend) << ParticleFlag::BlendShift;
            if (flipbook->GetClip().Loop)
            {
                flags |= ParticleFlag::Loop;
            }
            if (emitter.RandomStart)
            {
                flags |= ParticleFlag::RandomStart;
            }

            const Texture& atlas = *flipbook->GetAtlas();
            record.Textures.x = atlas.GetHandle().Index;
            record.Grid = uvec4(flipbook->GetColumns(), flipbook->GetRows(),
                                flipbook->GetClip().FrameCount, atlas.GetSamplerHandle().Index);
            record.Counts.z = static_cast<u32>(flipbook->GetAlphaMode());
            if (blend == FlipbookFrameBlend::Motion)
            {
                record.Textures.z = flipbook->GetMotion()->GetHandle().Index;
                record.Playback.y = flipbook->GetMotionScale();
            }
            if (sixWay)
            {
                flags |= ParticleFlag::SixWay;
                record.Textures.y = flipbook->GetLightB()->GetHandle().Index;
                record.Textures.w = flipbook->GetTemperatureTexture()->GetHandle().Index;
                if (flipbook->GetTemperatureEncoding() == FlipbookTemperatureEncoding::Sqrt)
                {
                    flags |= ParticleFlag::SqrtTemperature;
                }
                if (flipbook->GetTemperatureChannel() == 0)
                {
                    flags |= ParticleFlag::TemperatureRed;
                }
                const FlipbookEmission& emission =
                    emitter.Emission ? *emitter.Emission : flipbook->GetEmission();
                const auto keys = static_cast<u32>(
                    std::min<usize>(emission.Ramp.size(), MaxFlipbookEmissionKeys));
                for (u32 key = 0; key < keys; ++key)
                {
                    record.Ramp.Keys[key] = vec4(emission.Ramp[key].Color, emission.Ramp[key].T);
                }
                record.Ramp.Params = vec4(emission.Ignition, static_cast<f32>(keys), 0.0f, 0.0f);
                if (keys > 0)
                {
                    flags |= ParticleFlag::Emits;
                }
            }

            const f32 rate =
                emitter.Playback == ParticlePlayback::Fps
                    ? (emitter.PlaybackFps > 0.0f ? emitter.PlaybackFps : flipbook->GetClip().Fps)
                    : 0.0f;
            // A fixed-rate emitter whose rate resolves to nothing holds its first frame, as a
            // lifetime-stretched one would not; a tiny rate stands in for "never advances".
            record.Playback.x =
                emitter.Playback == ParticlePlayback::Fps ? std::max(rate, 1e-6f) : 0.0f;
            record.Playback.w = flipbook->GetAspect();
            record.Frame =
                vec4(flipbook->GetAnchor(), 1.0f / static_cast<f32>(flipbook->GetColumns()),
                     1.0f / static_cast<f32>(flipbook->GetRows()));
            const uvec2 texels =
                uvec2(flipbook->GetColumns(), flipbook->GetRows()) * flipbook->GetFrameSize();
            record.Inset = vec4(0.5f / static_cast<f32>(texels.x),
                                0.5f / static_cast<f32>(texels.y), 0.0f, 0.0f);

            // Only a flipbook recording extents carries a table; GetFrameScale reads 1 for one that
            // records none, which the shader's no-table path reproduces.
            const u32 frames = flipbook->GetClip().FrameCount;
            bool records = false;
            for (u32 frame = 0; frame < frames && !records; ++frame)
            {
                records = flipbook->GetFrameScale(frame) != 1.0f;
            }
            if (records && frameScales.size() + frames <= MaxParticleFrameScalesPerFrame)
            {
                record.Counts.w = static_cast<u32>(frameScales.size());
                for (u32 frame = 0; frame < frames; ++frame)
                {
                    frameScales.push_back(flipbook->GetFrameScale(frame));
                }
            }
        }

        const bool additive = IsAdditive(emitter, flipbook);
        record.Mode = uvec4(static_cast<u32>(emitter.Render), additive ? 1u : 0u,
                            static_cast<u32>(emitter.Shape), flags);
        u32 rateTotal = 0;
        if (emitter.RatePerSecond > 0.0f)
        {
            rateTotal = emitter.RateDuration > 0.0f
                            ? static_cast<u32>(std::ceil(static_cast<f64>(emitter.RateDuration) *
                                                         emitter.RatePerSecond))
                            : UnboundedRate;
        }
        record.Counts.x = emitter.BurstCount;
        record.Counts.y = rateTotal;
        record.Spawn = vec4(emitter.BurstTime, emitter.RateStart, emitter.RatePerSecond, 0.0f);
        record.LifetimeSize = vec4(emitter.Lifetime, emitter.Size);
        record.Speed = vec4(spawn.Speed, spawn.SpeedByRadius, spawn.Radius);
        record.Cone = vec4(spawn.ConeAxis, spawn.ConeCos);

        f32 soft = 0.0f;
        if (billboard)
        {
            soft = emitter.SoftDistance ? std::max(*emitter.SoftDistance, 0.0f)
                                        : (sixWay ? -1.0f : 0.0f);
        }
        record.Noise = vec4(spawn.NoiseAmplitude, spawn.NoiseFrequency, soft, 1.0f);
        record.Accel = vec4(emitter.Acceleration, emitter.Drag);
        record.Rotation = vec4(glm::radians(emitter.Rotation), spawn.Spin);
        record.Streak = vec4(emitter.StreakColor, emitter.StreakLengthScale);
        const std::span<const vec4> lut = effect.GetEmitterLut(emitterIndex);
        std::ranges::copy(lut, record.Lut.begin());
        return record;
    }

    void ParticleGather::Reset()
    {
        // A slot kept but freed carries its bumped generation, so any particle still on the GPU
        // dies at the next update.
        for (Slot& slot : m_Slots)
        {
            if (slot.Used)
            {
                ++slot.Generation;
            }
            slot.Used = false;
            slot.Cursors.clear();
        }
    }

    void ParticleGather::Gather(const Scene& scene, const dvec3& eye, const u32 capacity,
                                ParticleFramePlan& plan, const u32 visibleLayers,
                                const Entity exclude)
    {
        plan = ParticleFramePlan{};
        if (scene.GetInstanceSerial() != m_SceneSerial)
        {
            Reset();
            m_SceneSerial = scene.GetInstanceSerial();
        }

        std::unordered_map<u64, u32> bySlotKey;
        for (u32 index = 0; index < m_Slots.size(); ++index)
        {
            m_Slots[index].Seen = false;
            if (m_Slots[index].Used)
            {
                bySlotKey.emplace(KeyOf(m_Slots[index].Owner, m_Slots[index].Playback), index);
            }
        }

        // The instances this frame tracks, by slot, with the component each reads and its feed.
        struct Tracked
        {
            u32 Slot = 0;
            const ParticleEffectInstance* Instance = nullptr;
            const ParticleFeed* Feed = nullptr;
            // Simulated but not drawn in this view.
            bool Masked = false;
        };
        vector<Tracked> tracked;
        for (const auto [entity, instance] : scene.View<ParticleEffectInstance>())
        {
            if (instance.Effect.Get() == nullptr || !instance.Anchored || instance.Finished)
            {
                continue;
            }
            const u64 key = KeyOf(entity, instance.Playback);
            u32 slotIndex = 0;
            if (const auto found = bySlotKey.find(key);
                found != bySlotKey.end() && m_Slots[found->second].Owner == entity &&
                m_Slots[found->second].Playback == instance.Playback)
            {
                slotIndex = found->second;
            }
            else
            {
                const auto free =
                    std::ranges::find_if(m_Slots, [](const Slot& s) { return !s.Used; });
                if (free != m_Slots.end())
                {
                    slotIndex = static_cast<u32>(free - m_Slots.begin());
                }
                else if (m_Slots.size() < MaxParticleInstances)
                {
                    slotIndex = static_cast<u32>(m_Slots.size());
                    m_Slots.emplace_back();
                }
                else
                {
                    ++plan.InstancesDropped;
                    continue;
                }
                Slot& claimed = m_Slots[slotIndex];
                claimed.Owner = entity;
                claimed.Playback = instance.Playback;
                claimed.Used = true;
                ++claimed.Generation;
                claimed.Cursors.assign(instance.Effect.Get()->GetEmitters().size(),
                                       ParticleStreamCounts{});
                claimed.FedCursor = 0;
                bySlotKey[key] = slotIndex;
            }
            Slot& slot = m_Slots[slotIndex];
            if (slot.Seen)
            {
                // Two components carrying one key — a cloned entity mid-copy — draw once.
                continue;
            }
            slot.Seen = true;
            slot.Cursors.resize(instance.Effect.Get()->GetEmitters().size());
            tracked.push_back(
                {.Slot = slotIndex,
                 .Instance = &instance,
                 .Feed = scene.TryGet<ParticleFeed>(entity),
                 .Masked = entity == exclude || !RenderLayerInMask(visibleLayers, instance.Layer)});
        }

        for (Slot& slot : m_Slots)
        {
            if (slot.Used && !slot.Seen)
            {
                slot.Used = false;
                ++slot.Generation;
                slot.Cursors.clear();
            }
        }

        // Every slot is written: a free one draws nothing, and its bumped generation kills
        // whatever particles it left.
        plan.Instances.resize(m_Slots.size());
        for (u32 index = 0; index < m_Slots.size(); ++index)
        {
            plan.Instances[index].Generation =
                static_cast<u32>(m_Slots[index].Generation) | ParticleInstanceHidden;
        }
        plan.InstanceCount = static_cast<u32>(tracked.size());

        std::unordered_map<const ParticleEffect*, u32> effectBase;
        const auto* origin = scene.TryGetFirst<FloatingOrigin>();
        const dvec3 originPosition = origin != nullptr ? origin->Position : dvec3(0.0);
        u64 alive = 0;
        u64 alphaAlive = 0;

        for (const auto& [slotIndex, instance, feed, masked] : tracked)
        {
            Slot& slot = m_Slots[slotIndex];
            const ParticleEffect& effect = *instance->Effect.Get();
            const u32 emitterCount = static_cast<u32>(effect.GetEmitters().size());

            // The effect's emitters, once per frame however many instances play it. An effect
            // not drawn is either past the table or waiting on a flipbook still loading.
            constexpr u32 OverBudget = std::numeric_limits<u32>::max();
            constexpr u32 Waiting = OverBudget - 1u;
            bool drawn = true;
            bool waiting = false;
            u32 base = 0;
            if (const auto found = effectBase.find(&effect); found != effectBase.end())
            {
                base = found->second;
                drawn = base < Waiting;
                waiting = base == Waiting;
            }
            else
            {
                // Each emitter's own record plays its first flipbook; the records for its further
                // flipbooks follow every emitter's own, so an emitter's index is its record's.
                u32 recordCount = emitterCount;
                for (const ParticleEmitterDesc& emitter : effect.GetEmitters())
                {
                    recordCount += VariantCount(emitter) - 1u;
                }
                if (plan.Emitters.size() + recordCount > MaxParticleEmittersPerFrame)
                {
                    drawn = false;
                }
                base = static_cast<u32>(plan.Emitters.size());
                for (u32 e = 0; e < emitterCount && drawn; ++e)
                {
                    const optional<GpuParticleEmitter> record =
                        BuildParticleEmitter(effect, e, 0u, plan.FrameScales);
                    if (!record)
                    {
                        drawn = false;
                        waiting = true;
                        break;
                    }
                    plan.Emitters.push_back(*record);
                }
                for (u32 e = 0; e < emitterCount && drawn; ++e)
                {
                    const u32 variants = VariantCount(effect.GetEmitters()[e]);
                    plan.Emitters[base + e].Variants =
                        uvec4(variants, static_cast<u32>(plan.Emitters.size()) - base, 0u, 0u);
                    for (u32 v = 1; v < variants && drawn; ++v)
                    {
                        const optional<GpuParticleEmitter> record =
                            BuildParticleEmitter(effect, e, v, plan.FrameScales);
                        if (!record)
                        {
                            drawn = false;
                            waiting = true;
                            break;
                        }
                        plan.Emitters.push_back(*record);
                    }
                }
                if (!drawn)
                {
                    plan.Emitters.resize(base);
                }
                effectBase.emplace(&effect, drawn ? base : (waiting ? Waiting : OverBudget));
            }
            if (!drawn)
            {
                ++(waiting ? plan.InstancesWaiting : plan.EmittersDropped);
            }

            GpuParticleInstance& record = plan.Instances[slotIndex];
            record.AnchorEye = vec3(instance->Anchor - originPosition - eye);
            record.Time = instance->Time;
            record.Orientation = vec4(instance->Orientation.x, instance->Orientation.y,
                                      instance->Orientation.z, instance->Orientation.w);
            record.InheritVelocity = instance->InheritVelocity;
            record.Seed = instance->Seed;
            record.Generation = static_cast<u32>(slot.Generation) |
                                (drawn && !masked ? 0u : ParticleInstanceHidden);
            record.EmitterBase = drawn ? base : 0u;
            record.EmitterCount = drawn ? emitterCount : 0u;
            record.Scale = instance->Scale;

            // Fed spawns, in sequence, from where this renderer left off: each spawn at or before
            // the playback time and still alive is issued once, a run of one emitter's spawns
            // sharing a range; one past the budgets is passed over and counted.
            vector<u64> fedAlive(emitterCount, 0);
            vector<u32> fedIssued(emitterCount, 0);
            if (feed != nullptr)
            {
                const auto& emitters = effect.GetEmitters();
                const u64 end = feed->First + feed->Spawns.size();
                u64 sequence = std::max(slot.FedCursor, feed->First);
                for (; drawn && sequence < end; ++sequence)
                {
                    const ParticleFeedSpawn& spawn = feed->Spawns[sequence - feed->First];
                    if (spawn.Time > instance->Time)
                    {
                        break;
                    }
                    if (spawn.Emitter >= emitterCount || !emitters[spawn.Emitter].Fed ||
                        spawn.Time + emitters[spawn.Emitter].Lifetime.y <= instance->Time)
                    {
                        continue;
                    }
                    if (plan.SpawnTotal >= capacity ||
                        plan.FedSpawns.size() >= MaxParticleFedSpawnsPerFrame)
                    {
                        ++plan.SpawnsDropped;
                        continue;
                    }
                    const u32 key = slotIndex | (spawn.Emitter << 16) | ParticleRangeFed;
                    const u32 record = static_cast<u32>(plan.FedSpawns.size());
                    GpuParticleSpawnRange* const last =
                        plan.Ranges.empty() ? nullptr : &plan.Ranges.back();
                    if (last == nullptr || last->InstanceEmitter != key ||
                        last->FirstIndex + last->Count != record)
                    {
                        if (plan.Ranges.size() >= MaxParticleSpawnRanges)
                        {
                            ++plan.SpawnsDropped;
                            continue;
                        }
                        plan.Ranges.push_back({.InstanceEmitter = key,
                                               .FirstIndex = record,
                                               .Count = 0,
                                               .Prefix = plan.SpawnTotal});
                    }
                    plan.FedSpawns.push_back({
                        .Offset = vec3(spawn.Position - instance->Anchor),
                        .Time = spawn.Time,
                        .Velocity = spawn.Velocity,
                        .Index = static_cast<u32>(sequence),
                    });
                    ++plan.Ranges.back().Count;
                    ++plan.SpawnTotal;
                    ++fedIssued[spawn.Emitter];
                }
                if (drawn)
                {
                    slot.FedCursor = sequence;
                }
                for (const ParticleFeedSpawn& spawn : feed->Spawns)
                {
                    if (spawn.Emitter < emitterCount && emitters[spawn.Emitter].Fed)
                    {
                        ++fedAlive[spawn.Emitter];
                    }
                }
            }

            for (u32 e = 0; e < emitterCount; ++e)
            {
                const ParticleEmitterDesc& emitter = effect.GetEmitters()[e];
                ParticleStreamCounts& cursor = slot.Cursors[e];
                const ParticleStreamCounts first =
                    ParticleFirstPossiblyAlive(emitter, instance->Time);
                const u32 issuedBefore = plan.SpawnTotal;
                if (drawn)
                {
                    const ParticleStreamCounts end =
                        ParticleSpawnedBy(emitter, instance->Time, instance->StopTime);
                    const auto issue = [&](const u32 from, const u32 to, const u32 indexBase)
                    {
                        if (to <= from)
                        {
                            return;
                        }
                        u32 count = to - from;
                        const u32 room =
                            capacity > plan.SpawnTotal ? capacity - plan.SpawnTotal : 0u;
                        if (count > room || plan.Ranges.size() >= MaxParticleSpawnRanges)
                        {
                            const u32 kept =
                                plan.Ranges.size() >= MaxParticleSpawnRanges ? 0u : room;
                            plan.SpawnsDropped += count - kept;
                            count = kept;
                        }
                        if (count == 0)
                        {
                            return;
                        }
                        plan.Ranges.push_back({
                            .InstanceEmitter = slotIndex | (e << 16),
                            .FirstIndex = indexBase + from,
                            .Count = count,
                            .Prefix = plan.SpawnTotal,
                        });
                        plan.SpawnTotal += count;
                    };
                    issue(std::max(cursor.Burst, first.Burst), end.Burst, 0u);
                    issue(std::max(cursor.Rate, first.Rate), end.Rate, emitter.BurstCount);
                    cursor.Burst = std::max(cursor.Burst, end.Burst);
                    cursor.Rate = std::max(cursor.Rate, end.Rate);
                }

                const u64 live = static_cast<u64>(CountsBetween(cursor.Burst, first.Burst)) +
                                 CountsBetween(cursor.Rate, first.Rate) + fedAlive[e];
                alive += live;
                if (!drawn)
                {
                    continue;
                }
                // A particle may play any of the emitter's flipbooks, so its records are bounded
                // together: alpha when any is, depth-reading when any is.
                const GpuParticleEmitter& own = plan.Emitters[base + e];
                bool alpha = own.Mode.y == 0u;
                bool soft = own.Noise.z != 0.0f;
                for (u32 v = 1; v < own.Variants.x; ++v)
                {
                    const GpuParticleEmitter& other = plan.Emitters[base + own.Variants.y + v - 1u];
                    alpha = alpha || other.Mode.y == 0u;
                    soft = soft || other.Noise.z != 0.0f;
                }
                if (alpha)
                {
                    alphaAlive += live;
                    plan.AlphaSpawnTotal += plan.SpawnTotal - issuedBefore + fedIssued[e];
                }
                if (soft && !masked)
                {
                    plan.ReadsSceneDepth = true;
                }
            }
        }

        plan.AliveBound = static_cast<u32>(std::min<u64>(alive, capacity));
        plan.AlphaBound = static_cast<u32>(std::min<u64>(alphaAlive, capacity));
    }
}
