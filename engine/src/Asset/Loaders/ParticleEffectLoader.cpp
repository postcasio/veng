#include "ParticleEffectLoader.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>

namespace Veng
{
    namespace
    {
        AssetLoadError Corrupt(const AssetId id, string detail)
        {
            return AssetLoadError{
                .Kind = AssetError::Corrupt, .Id = id, .Detail = std::move(detail)};
        }

        // Reads a curve's run out of the key pool; false when it overruns the pool, holds more than
        // a curve may, or its keys are out of order or outside [0, 1].
        template <class Key>
        bool ReadCurve(const CookedParticleCurve& curve, const vector<f32>& pool, vector<Key>& out)
        {
            constexpr u32 Stride = sizeof(Key) / sizeof(f32);
            if (curve.KeyCount > MaxParticleCurveKeys ||
                static_cast<u64>(curve.KeyOffset) + static_cast<u64>(curve.KeyCount) * Stride >
                    pool.size())
            {
                return false;
            }
            out.resize(curve.KeyCount);
            for (u32 k = 0; k < curve.KeyCount; ++k)
            {
                std::memcpy(&out[k], pool.data() + curve.KeyOffset + k * Stride, sizeof(Key));
                if (!(out[k].T >= 0.0f && out[k].T <= 1.0f) ||
                    (k > 0 && !(out[k].T > out[k - 1].T)))
                {
                    return false;
                }
            }
            return true;
        }

        vec2 Range(const f32 (&values)[2])
        {
            return {values[0], values[1]};
        }
    }

    AssetResult<Detail::ParsedAsset>
    ParticleEffectLoader::Parse(const AssetParseContext& /*context*/, const AssetId id,
                                const std::span<const u8> cooked) const
    {
        if (cooked.size() < sizeof(CookedParticleEffectHeader))
        {
            return std::unexpected(
                Corrupt(id, "particle effect: cooked blob smaller than its header"));
        }
        CookedParticleEffectHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));
        if (header.Version != CookedParticleEffectVersion)
        {
            return std::unexpected(Corrupt(
                id, fmt::format("particle effect: cooked version {} != CookedParticleEffectVersion "
                                "{} — re-cook the pack",
                                header.Version, CookedParticleEffectVersion)));
        }
        if (header.EmitterCount > MaxParticleEmitters || header.FlashCount > MaxParticleFlashes)
        {
            return std::unexpected(
                Corrupt(id, fmt::format("particle effect: {} emitters and {} flashes exceed the "
                                        "{} and {} an effect holds",
                                        header.EmitterCount, header.FlashCount, MaxParticleEmitters,
                                        MaxParticleFlashes)));
        }
        const usize emitterBytes =
            static_cast<usize>(header.EmitterCount) * sizeof(CookedParticleEmitter);
        const usize flashBytes =
            static_cast<usize>(header.FlashCount) * sizeof(CookedParticleFlash);
        const usize flipbookBytes = static_cast<usize>(header.FlipbookIds) * sizeof(u64);
        const usize keyBytes = static_cast<usize>(header.KeyFloats) * sizeof(f32);
        if (cooked.size() < sizeof(header) + emitterBytes + flashBytes + flipbookBytes + keyBytes)
        {
            return std::unexpected(Corrupt(id, "particle effect: cooked blob truncated"));
        }

        usize offset = sizeof(header);
        vector<CookedParticleEmitter> emitters(header.EmitterCount);
        std::memcpy(emitters.data(), cooked.data() + offset, emitterBytes);
        offset += emitterBytes;
        vector<CookedParticleFlash> flashes(header.FlashCount);
        std::memcpy(flashes.data(), cooked.data() + offset, flashBytes);
        offset += flashBytes;
        vector<u64> flipbooks(header.FlipbookIds);
        std::memcpy(flipbooks.data(), cooked.data() + offset, flipbookBytes);
        offset += flipbookBytes;
        vector<f32> pool(header.KeyFloats);
        std::memcpy(pool.data(), cooked.data() + offset, keyBytes);

        const auto info = CreateRef<ParticleEffectInfo>();
        info->Name = fmt::format("ParticleEffect {}", id.Value);
        Detail::ParsedAsset parsed;
        // The dependencies each emitter takes its flipbooks from, in order; none for a streak.
        vector<vector<usize>> flipbookDependencies;
        for (const CookedParticleEmitter& source : emitters)
        {
            if (source.Render > static_cast<u32>(ParticleRender::Streak) ||
                source.Blend > static_cast<u32>(ParticleBlend::Additive) ||
                source.Shape > static_cast<u32>(ParticleShape::Shell) ||
                source.PlaybackMode > static_cast<u32>(ParticlePlayback::Fps))
            {
                return std::unexpected(
                    Corrupt(id, "particle effect: unrecognized enumerated value in an emitter"));
            }
            ParticleEmitterDesc& emitter = info->Emitters.emplace_back();
            emitter.Render = static_cast<ParticleRender>(source.Render);
            emitter.Blend = static_cast<ParticleBlend>(source.Blend);
            emitter.BurstCount = source.BurstCount;
            emitter.BurstTime = source.BurstTime;
            emitter.RatePerSecond = source.RatePerSecond;
            emitter.RateStart = source.RateStart;
            emitter.RateDuration = source.RateDuration;
            emitter.Lifetime = Range(source.Lifetime);
            emitter.Shape = static_cast<ParticleShape>(source.Shape);
            emitter.ShapeRadius = source.ShapeRadius;
            emitter.Speed = Range(source.Speed);
            emitter.ConeAxis = vec3(source.ConeAxis[0], source.ConeAxis[1], source.ConeAxis[2]);
            emitter.ConeAngle = source.ConeAngle;
            emitter.SpeedByRadius = source.SpeedByRadius;
            emitter.NoiseAmplitude = source.NoiseAmplitude;
            emitter.NoiseFrequency = source.NoiseFrequency;
            emitter.Acceleration =
                vec3(source.Acceleration[0], source.Acceleration[1], source.Acceleration[2]);
            emitter.Drag = source.Drag;
            emitter.Size = Range(source.Size);
            emitter.Rotation = Range(source.Rotation);
            emitter.Spin = Range(source.Spin);
            emitter.Playback = static_cast<ParticlePlayback>(source.PlaybackMode);
            emitter.PlaybackFps = source.PlaybackFps;
            emitter.RandomStart = source.RandomStart != 0;
            if (source.HasSoftDistance != 0)
            {
                emitter.SoftDistance = source.SoftDistance;
            }
            emitter.StreakColor =
                vec3(source.StreakColor[0], source.StreakColor[1], source.StreakColor[2]);
            emitter.StreakLengthScale = source.StreakLengthScale;

            vector<ParticleColorKey> rampKeys;
            const bool curvesValid =
                ReadCurve(source.SizeCurve, pool, emitter.SizeCurve) &&
                ReadCurve(source.OpacityCurve, pool, emitter.OpacityCurve) &&
                ReadCurve(source.TintCurve, pool, emitter.TintCurve) &&
                ReadCurve(source.IntensityCurve, pool, emitter.IntensityCurve) &&
                ReadCurve(source.TemperatureCurve, pool, emitter.TemperatureCurve) &&
                ReadCurve(source.EmissionRamp, pool, rampKeys);
            if (!curvesValid || rampKeys.size() > MaxFlipbookEmissionKeys)
            {
                return std::unexpected(
                    Corrupt(id, "particle effect: a curve overruns the key pool or is unsorted"));
            }
            if (source.HasEmission != 0)
            {
                FlipbookEmission emission{.Ignition = source.Ignition};
                for (const ParticleColorKey& key : rampKeys)
                {
                    emission.Ramp.push_back({.T = key.T, .Color = key.Value});
                }
                emitter.Emission = std::move(emission);
            }
            if (!(emitter.Lifetime.x <= emitter.Lifetime.y && emitter.Size.x <= emitter.Size.y &&
                  emitter.Speed.x <= emitter.Speed.y && emitter.Rotation.x <= emitter.Rotation.y &&
                  emitter.Spin.x <= emitter.Spin.y))
            {
                return std::unexpected(
                    Corrupt(id, "particle effect: an emitter range has min above max"));
            }

            const bool billboard = emitter.Render == ParticleRender::Billboard;
            const bool runValid =
                source.FlipbookCount <= MaxParticleFlipbookVariants &&
                static_cast<u64>(source.FlipbookOffset) + source.FlipbookCount <= flipbooks.size();
            if (!runValid || billboard != (source.FlipbookCount != 0) ||
                (!billboard && emitter.Blend == ParticleBlend::Asset))
            {
                return std::unexpected(Corrupt(
                    id, "particle effect: a billboard without a flipbook, a streak with one, or a "
                        "flipbook run outside the pool"));
            }
            vector<usize>& dependencies = flipbookDependencies.emplace_back();
            for (u32 v = 0; v < source.FlipbookCount; ++v)
            {
                const u64 flipbook = flipbooks[source.FlipbookOffset + v];
                if (flipbook == 0)
                {
                    return std::unexpected(
                        Corrupt(id, "particle effect: a billboard names the null flipbook id"));
                }
                const auto existing = std::ranges::find_if(
                    parsed.Dependencies, [flipbook](const Detail::AssetDependency& dependency)
                    { return dependency.Id.Value == flipbook; });
                dependencies.push_back(static_cast<usize>(existing - parsed.Dependencies.begin()));
                if (existing == parsed.Dependencies.end())
                {
                    parsed.Dependencies.push_back(
                        {.Type = AssetTypes::Flipbook, .Id = AssetId{flipbook}});
                }
            }
        }

        for (const CookedParticleFlash& source : flashes)
        {
            ParticleFlashDesc& flash = info->Flashes.emplace_back();
            flash.Time = source.Time;
            flash.Duration = source.Duration;
            flash.Color = vec3(source.Color[0], source.Color[1], source.Color[2]);
            flash.Intensity = source.Intensity;
            flash.Range = source.Range;
            flash.Radius = source.Radius;
            if (!ReadCurve(source.Curve, pool, flash.Curve))
            {
                return std::unexpected(Corrupt(
                    id, "particle effect: a flash curve overruns the key pool or is unsorted"));
            }
        }

        parsed.Complete = [info, flipbookDependencies = std::move(flipbookDependencies)](
                              AssetManager& /*manager*/,
                              const std::span<const Ref<Detail::AssetCacheEntry>> resolved)
            -> AssetResult<Detail::LoadJob>
        {
            for (usize e = 0; e < flipbookDependencies.size(); ++e)
            {
                for (const usize dependency : flipbookDependencies[e])
                {
                    info->Emitters[e].Flipbooks.push_back(
                        AssetManager::HandleOf<Flipbook>(resolved[dependency]));
                }
            }
            return Detail::LoadJob{
                .Resource = Detail::RefAny(ParticleEffect::Create(*info)),
                .Dependencies =
                    vector<Ref<Detail::AssetCacheEntry>>(resolved.begin(), resolved.end()),
            };
        };
        return parsed;
    }
}
