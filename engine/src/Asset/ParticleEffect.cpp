#include <Veng/Asset/ParticleEffect.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include <Veng/Assert.h>

namespace Veng
{
    namespace
    {
        // A rate emitter with no duration spawns until its instance stops emitting.
        constexpr u32 UnboundedRate = std::numeric_limits<u32>::max();

        // The rate particles an emitter's duration allows; UnboundedRate for none.
        u32 RateTotal(const ParticleEmitterDesc& emitter)
        {
            if (emitter.RatePerSecond <= 0.0f)
            {
                return 0;
            }
            if (emitter.RateDuration <= 0.0f)
            {
                return UnboundedRate;
            }
            const f64 count = std::ceil(static_cast<f64>(emitter.RateDuration) *
                                        static_cast<f64>(emitter.RatePerSecond));
            return static_cast<u32>(std::min(count, static_cast<f64>(UnboundedRate - 1)));
        }

        f32 RateSpawnTime(const ParticleEmitterDesc& emitter, const u32 j)
        {
            return emitter.RateStart + static_cast<f32>(j) / emitter.RatePerSecond;
        }

        // The rate particles j < total whose spawn time is at most t (strictly below it when
        // inclusive is false): an estimate from the rate, then corrected against RateSpawnTime
        // itself, so the count agrees with the times the particles are given.
        u32 RateCountBefore(const ParticleEmitterDesc& emitter, const f32 t, const bool inclusive)
        {
            const u32 total = RateTotal(emitter);
            if (total == 0)
            {
                return 0;
            }
            const auto spawned = [&](const u32 j)
            {
                const f32 time = RateSpawnTime(emitter, j);
                return inclusive ? time <= t : time < t;
            };
            const f64 estimate = std::floor((static_cast<f64>(t) - emitter.RateStart) *
                                            static_cast<f64>(emitter.RatePerSecond)) +
                                 1.0;
            u32 count = static_cast<u32>(
                std::clamp(estimate, 0.0, static_cast<f64>(std::min(total, UnboundedRate - 1))));
            while (count > 0 && !spawned(count - 1))
            {
                --count;
            }
            while (count < total && count < UnboundedRate - 1 && spawned(count))
            {
                ++count;
            }
            return count;
        }

        template <class Key, class Value>
        Value EvaluateCurve(const std::span<const Key> keys, const f32 t, const Value fallback)
        {
            if (keys.empty())
            {
                return fallback;
            }
            if (t <= keys.front().T)
            {
                return keys.front().Value;
            }
            if (t >= keys.back().T)
            {
                return keys.back().Value;
            }
            for (usize k = 1; k < keys.size(); ++k)
            {
                if (t <= keys[k].T)
                {
                    const Key& a = keys[k - 1];
                    const Key& b = keys[k];
                    const f32 f = (t - a.T) / (b.T - a.T);
                    return a.Value + (b.Value - a.Value) * f;
                }
            }
            return keys.back().Value;
        }

        template <class Key>
        bool CurveIsValid(const vector<Key>& keys)
        {
            if (keys.size() > MaxParticleCurveKeys)
            {
                return false;
            }
            for (usize k = 0; k < keys.size(); ++k)
            {
                if (!(keys[k].T >= 0.0f && keys[k].T <= 1.0f) ||
                    (k > 0 && !(keys[k].T > keys[k - 1].T)))
                {
                    return false;
                }
            }
            return true;
        }
    }

    f32 EvaluateParticleCurve(const std::span<const ParticleCurveKey> keys, const f32 t,
                              const f32 fallback)
    {
        return EvaluateCurve<ParticleCurveKey, f32>(keys, t, fallback);
    }

    vec3 EvaluateParticleCurve(const std::span<const ParticleColorKey> keys, const f32 t,
                               const vec3 fallback)
    {
        return EvaluateCurve<ParticleColorKey, vec3>(keys, t, fallback);
    }

    Ref<ParticleEffect> ParticleEffect::Create(const ParticleEffectInfo& info)
    {
        VE_ASSERT(info.Emitters.size() <= MaxParticleEmitters,
                  "ParticleEffect '{}': {} emitters exceed the {} an effect holds", info.Name,
                  info.Emitters.size(), MaxParticleEmitters);
        VE_ASSERT(info.Flashes.size() <= MaxParticleFlashes,
                  "ParticleEffect '{}': {} flashes exceed the {} an effect holds", info.Name,
                  info.Flashes.size(), MaxParticleFlashes);
        for (const ParticleEmitterDesc& emitter : info.Emitters)
        {
            VE_ASSERT(CurveIsValid(emitter.SizeCurve) && CurveIsValid(emitter.OpacityCurve) &&
                          CurveIsValid(emitter.TintCurve) && CurveIsValid(emitter.IntensityCurve) &&
                          CurveIsValid(emitter.TemperatureCurve),
                      "ParticleEffect '{}': emitter '{}' has a curve with unsorted or "
                      "out-of-range keys, or more than {}",
                      info.Name, emitter.Name, MaxParticleCurveKeys);
            VE_ASSERT(emitter.Lifetime.x <= emitter.Lifetime.y &&
                          emitter.Size.x <= emitter.Size.y && emitter.Speed.x <= emitter.Speed.y &&
                          emitter.Rotation.x <= emitter.Rotation.y &&
                          emitter.Spin.x <= emitter.Spin.y,
                      "ParticleEffect '{}': emitter '{}' has a range with min above max", info.Name,
                      emitter.Name);
            VE_ASSERT(emitter.Render != ParticleRender::Streak ||
                          emitter.Blend != ParticleBlend::Asset,
                      "ParticleEffect '{}': streak emitter '{}' must choose alpha or additive",
                      info.Name, emitter.Name);
            VE_ASSERT(
                emitter.Flipbooks.size() <= MaxParticleFlipbookVariants &&
                    (emitter.Render == ParticleRender::Billboard || emitter.Flipbooks.empty()),
                "ParticleEffect '{}': emitter '{}' names {} flipbooks; a billboard names at "
                "most {} and a streak none",
                info.Name, emitter.Name, emitter.Flipbooks.size(), MaxParticleFlipbookVariants);
        }
        for (const ParticleFlashDesc& flash : info.Flashes)
        {
            VE_ASSERT(CurveIsValid(flash.Curve),
                      "ParticleEffect '{}': a flash curve has unsorted or out-of-range keys",
                      info.Name);
        }
        return Ref<ParticleEffect>(new ParticleEffect(info));
    }

    ParticleEffect::ParticleEffect(ParticleEffectInfo info) : m_Info(std::move(info))
    {
        m_Luts.resize(m_Info.Emitters.size());
        for (usize e = 0; e < m_Info.Emitters.size(); ++e)
        {
            const ParticleEmitterDesc& emitter = m_Info.Emitters[e];
            EmitterLut& lut = m_Luts[e];
            for (u32 i = 0; i < ParticleLutSamples; ++i)
            {
                // The last sample is exactly 1, so it reads the last key, not a rounding short of it.
                const f32 u = i + 1 == ParticleLutSamples
                                  ? 1.0f
                                  : static_cast<f32>(i) / static_cast<f32>(ParticleLutSamples - 1);
                lut[i] = vec4(EvaluateParticleCurve(emitter.SizeCurve, u, 1.0f),
                              EvaluateParticleCurve(emitter.OpacityCurve, u, 1.0f),
                              EvaluateParticleCurve(emitter.IntensityCurve, u, 1.0f),
                              EvaluateParticleCurve(emitter.TemperatureCurve, u, 0.0f));
                lut[ParticleLutSamples + i] =
                    vec4(EvaluateParticleCurve(emitter.TintCurve, u, vec3(1.0f)), 0.0f);
            }
        }
    }

    std::span<const vec4> ParticleEffect::GetEmitterLut(const u32 emitter) const
    {
        VE_ASSERT(emitter < m_Luts.size(), "ParticleEffect '{}': emitter {} out of range ({})",
                  m_Info.Name, emitter, m_Luts.size());
        return m_Luts[emitter];
    }

    f32 ParticleSpawnTime(const ParticleEmitterDesc& emitter, const u32 index)
    {
        if (index < emitter.BurstCount)
        {
            return emitter.BurstTime;
        }
        return RateSpawnTime(emitter, index - emitter.BurstCount);
    }

    ParticleStreamCounts ParticleSpawnedBy(const ParticleEmitterDesc& emitter, const f32 t,
                                           const f32 stopTime)
    {
        ParticleStreamCounts counts;
        if (emitter.BurstCount > 0 && emitter.BurstTime <= t)
        {
            counts.Burst = emitter.BurstCount;
        }
        counts.Rate = RateCountBefore(emitter, t, /*inclusive=*/true);
        if (stopTime >= 0.0f)
        {
            counts.Rate = std::min(counts.Rate, RateCountBefore(emitter, stopTime, false));
        }
        return counts;
    }

    ParticleStreamCounts ParticleFirstPossiblyAlive(const ParticleEmitterDesc& emitter, const f32 t)
    {
        const f32 cutoff = t - emitter.Lifetime.y;
        return {
            .Burst = emitter.BurstCount > 0 && emitter.BurstTime > cutoff ? 0u : emitter.BurstCount,
            .Rate = RateCountBefore(emitter, cutoff, /*inclusive=*/true),
        };
    }

    f32 ParticleEffectEndTime(const ParticleEffect& effect, const f32 stopTime)
    {
        f32 end = 0.0f;
        for (const ParticleEmitterDesc& emitter : effect.GetEmitters())
        {
            // A fed emitter takes particles until its instance stops, each fed no later than then.
            if (emitter.Fed)
            {
                if (stopTime < 0.0f)
                {
                    return std::numeric_limits<f32>::infinity();
                }
                end = std::max(end, stopTime + emitter.Lifetime.y);
                continue;
            }
            if (emitter.BurstCount > 0)
            {
                end = std::max(end, emitter.BurstTime + emitter.Lifetime.y);
            }
            u32 total = RateTotal(emitter);
            if (total == UnboundedRate && stopTime < 0.0f)
            {
                return std::numeric_limits<f32>::infinity();
            }
            if (stopTime >= 0.0f)
            {
                total = std::min(total, RateCountBefore(emitter, stopTime, /*inclusive=*/false));
            }
            if (total > 0)
            {
                end = std::max(end, RateSpawnTime(emitter, total - 1) + emitter.Lifetime.y);
            }
        }
        for (const ParticleFlashDesc& flash : effect.GetFlashes())
        {
            end = std::max(end, flash.Time + flash.Duration);
        }
        return end;
    }

    u32 ParticleEffectMaxAlive(const ParticleEffect& effect)
    {
        u64 total = 0;
        for (const ParticleEmitterDesc& emitter : effect.GetEmitters())
        {
            total += ParticleEmitterMaxAlive(emitter.BurstCount, emitter.RatePerSecond,
                                             emitter.Lifetime.y);
        }
        return static_cast<u32>(std::min<u64>(total, std::numeric_limits<u32>::max()));
    }
}
