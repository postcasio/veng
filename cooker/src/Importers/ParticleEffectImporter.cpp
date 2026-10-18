#include "ParticleEffectImporter.h"

#include <cstring>
#include <limits>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/HexId.h>
#include <Veng/Asset/ParticleEffect.h>
#include <Veng/Cook/JsonFile.h>
#include <Veng/Renderer/SceneRendererSettings.h>

namespace Veng::Cook
{
    namespace
    {
        constexpr const char* SchemaType = "particle-effect";
        constexpr i64 SchemaVersion = 1;

        // Reads one source file's fields, each failure located by the file and the field's path.
        class Reader
        {
        public:
            explicit Reader(string file) : m_File(std::move(file)) {}

            [[nodiscard]] string Error(const string& field, const string& message) const
            {
                return fmt::format("particle effect importer: '{}': {}: {}", m_File, field,
                                   message);
            }

            // A number, or `fallback` when absent.
            Result<f32> Number(const json& object, const char* key, const string& field,
                               const f32 fallback) const
            {
                if (!object.contains(key) || object[key].is_null())
                {
                    return fallback;
                }
                if (!object[key].is_number())
                {
                    return std::unexpected(Error(field, "must be a number"));
                }
                return object[key].get<f32>();
            }

            // A number that must not be negative.
            Result<f32> NonNegative(const json& object, const char* key, const string& field,
                                    const f32 fallback) const
            {
                const Result<f32> value = Number(object, key, field, fallback);
                if (value && *value < 0.0f)
                {
                    return std::unexpected(Error(field, "must not be negative"));
                }
                return value;
            }

            // A [min, max] range, min <= max.
            Result<vec2> Range(const json& object, const char* key, const string& field,
                               const vec2 fallback, const bool nonNegative) const
            {
                if (!object.contains(key) || object[key].is_null())
                {
                    return fallback;
                }
                const json& value = object[key];
                if (!value.is_array() || value.size() != 2 || !value[0].is_number() ||
                    !value[1].is_number())
                {
                    return std::unexpected(Error(field, "must be a [min, max] number pair"));
                }
                const vec2 range(value[0].get<f32>(), value[1].get<f32>());
                if (range.x > range.y)
                {
                    return std::unexpected(Error(field, "min exceeds max"));
                }
                if (nonNegative && range.x < 0.0f)
                {
                    return std::unexpected(Error(field, "must not be negative"));
                }
                return range;
            }

            // An [x, y, z] triple.
            Result<vec3> Triple(const json& object, const char* key, const string& field,
                                const vec3 fallback) const
            {
                if (!object.contains(key) || object[key].is_null())
                {
                    return fallback;
                }
                return TripleValue(object[key], field);
            }

            Result<vec3> TripleValue(const json& value, const string& field) const
            {
                if (!value.is_array() || value.size() != 3 || !value[0].is_number() ||
                    !value[1].is_number() || !value[2].is_number())
                {
                    return std::unexpected(Error(field, "must be an [x, y, z] number triple"));
                }
                return vec3(value[0].get<f32>(), value[1].get<f32>(), value[2].get<f32>());
            }

            // One of `names`, as its index; `fallback` when absent.
            Result<u32> Enum(const json& object, const char* key, const string& field,
                             const std::initializer_list<const char*> names,
                             const u32 fallback) const
            {
                if (!object.contains(key) || object[key].is_null())
                {
                    return fallback;
                }
                if (object[key].is_string())
                {
                    u32 index = 0;
                    for (const char* name : names)
                    {
                        if (object[key].get<string>() == name)
                        {
                            return index;
                        }
                        ++index;
                    }
                }
                string expected;
                for (const char* name : names)
                {
                    expected += fmt::format("{}\"{}\"", expected.empty() ? "" : ", ", name);
                }
                return std::unexpected(
                    Error(field, fmt::format("unknown value (expected {})", expected)));
            }

            // An optional object; an absent one reads as empty.
            Result<const json*> Object(const json& object, const char* key,
                                       const string& field) const
            {
                static const json Empty = json::object();
                if (!object.contains(key) || object[key].is_null())
                {
                    return &Empty;
                }
                if (!object[key].is_object())
                {
                    return std::unexpected(Error(field, "must be an object"));
                }
                return &object[key];
            }

        private:
            string m_File;
        };

        // The key pool every curve indexes, appended as the curves are read.
        struct KeyPool
        {
            vector<f32> Floats;

            CookedParticleCurve Append(const vector<f32>& keys, const u32 stride)
            {
                const CookedParticleCurve curve{
                    .KeyOffset = static_cast<u32>(Floats.size()),
                    .KeyCount = static_cast<u32>(keys.size() / stride),
                };
                Floats.insert(Floats.end(), keys.begin(), keys.end());
                return curve;
            }
        };

        // A curve of [t, value] (or [t, [r, g, b]] when colour) keys, strictly ascending in t over
        // [0, 1], at most maxKeys.
        Result<CookedParticleCurve> ReadCurve(const Reader& reader, const json& object,
                                              const char* key, const string& field,
                                              const bool colour, const u32 maxKeys, KeyPool& pool)
        {
            if (!object.contains(key) || object[key].is_null())
            {
                return CookedParticleCurve{};
            }
            const json& keys = object[key];
            if (!keys.is_array())
            {
                return std::unexpected(reader.Error(field, "must be an array of [t, value] keys"));
            }
            if (keys.size() > maxKeys)
            {
                return std::unexpected(
                    reader.Error(field, fmt::format("{} keys exceed the {} a curve holds",
                                                    keys.size(), maxKeys)));
            }
            vector<f32> floats;
            f32 previous = -1.0f;
            for (usize k = 0; k < keys.size(); ++k)
            {
                const string where = fmt::format("{}[{}]", field, k);
                const json& entry = keys[k];
                if (!entry.is_array() || entry.size() != 2 || !entry[0].is_number())
                {
                    return std::unexpected(reader.Error(where, "must be a [t, value] pair"));
                }
                const f32 t = entry[0].get<f32>();
                if (!(t >= 0.0f && t <= 1.0f))
                {
                    return std::unexpected(reader.Error(where, "t must lie in [0, 1]"));
                }
                if (!(t > previous))
                {
                    return std::unexpected(reader.Error(where, "keys must ascend strictly in t"));
                }
                previous = t;
                floats.push_back(t);
                if (colour)
                {
                    const Result<vec3> value = reader.TripleValue(entry[1], where);
                    if (!value)
                    {
                        return std::unexpected(value.error());
                    }
                    floats.insert(floats.end(), {value->x, value->y, value->z});
                }
                else
                {
                    if (!entry[1].is_number())
                    {
                        return std::unexpected(reader.Error(where, "value must be a number"));
                    }
                    floats.push_back(entry[1].get<f32>());
                }
            }
            return pool.Append(floats, colour ? 4u : 2u);
        }

        // Unwraps a Result into `out`, returning its error from the enclosing function.
#define VE_PFX_READ(out, expr)                                                                     \
    do                                                                                             \
    {                                                                                              \
        auto result_ = (expr);                                                                     \
        if (!result_)                                                                              \
        {                                                                                          \
            return std::unexpected(std::move(result_.error()));                                    \
        }                                                                                          \
        (out) = *result_;                                                                          \
    } while (false)

        Result<CookedParticleEmitter> ReadEmitter(const CookContext& context, const Reader& reader,
                                                  const json& source, const string& field,
                                                  KeyPool& pool)
        {
            if (!source.is_object())
            {
                return std::unexpected(reader.Error(field, "must be an object"));
            }
            CookedParticleEmitter emitter;
            VE_PFX_READ(emitter.Render, reader.Enum(source, "render", field + ".render",
                                                    {"billboard", "streak"}, 0));
            const bool billboard = emitter.Render == static_cast<u32>(ParticleRender::Billboard);
            VE_PFX_READ(emitter.Blend, reader.Enum(source, "blend", field + ".blend",
                                                   {"asset", "alpha", "additive"}, 0));
            if (!billboard && emitter.Blend == static_cast<u32>(ParticleBlend::Asset))
            {
                return std::unexpected(reader.Error(
                    field + ".blend", "a streak has no flipbook to take a blend from; choose "
                                      "\"alpha\" or \"additive\""));
            }

            const bool hasFlipbook = source.contains("flipbook") && !source["flipbook"].is_null();
            if (!billboard && hasFlipbook)
            {
                return std::unexpected(
                    reader.Error(field + ".flipbook", "a streak takes no flipbook"));
            }
            if (billboard)
            {
                if (!hasFlipbook || !source["flipbook"].is_string())
                {
                    return std::unexpected(reader.Error(
                        field + ".flipbook", "a billboard needs a flipbook id hex string"));
                }
                const optional<AssetId> id = ParseAssetId(source["flipbook"].get<string>());
                if (!id || !id->IsValid())
                {
                    return std::unexpected(reader.Error(
                        field + ".flipbook",
                        fmt::format("malformed id '{}'", source["flipbook"].get<string>())));
                }
                optional<ResolvedSource> resolved;
                if (context.Resolve)
                {
                    resolved = context.Resolve(*id);
                }
                if (!resolved || resolved->Type != AssetTypes::Flipbook)
                {
                    return std::unexpected(reader.Error(
                        field + ".flipbook",
                        fmt::format("{} does not resolve to a Flipbook in this pack or a reference "
                                    "pack",
                                    source["flipbook"].get<string>())));
                }
                emitter.Flipbook = id->Value;
            }

            const json* spawn = nullptr;
            VE_PFX_READ(spawn, reader.Object(source, "spawn", field + ".spawn"));
            const json* burst = nullptr;
            VE_PFX_READ(burst, reader.Object(*spawn, "burst", field + ".spawn.burst"));
            const json* rate = nullptr;
            VE_PFX_READ(rate, reader.Object(*spawn, "rate", field + ".spawn.rate"));
            if (burst->contains("count") && !(*burst)["count"].is_null())
            {
                if (!(*burst)["count"].is_number_integer() || (*burst)["count"].get<i64>() < 0 ||
                    (*burst)["count"].get<i64>() > std::numeric_limits<i32>::max())
                {
                    return std::unexpected(reader.Error(field + ".spawn.burst.count",
                                                        "must be a non-negative integer"));
                }
                emitter.BurstCount = static_cast<u32>((*burst)["count"].get<i64>());
            }
            VE_PFX_READ(emitter.BurstTime,
                        reader.NonNegative(*burst, "time", field + ".spawn.burst.time", 0.0f));
            VE_PFX_READ(
                emitter.RatePerSecond,
                reader.NonNegative(*rate, "perSecond", field + ".spawn.rate.perSecond", 0.0f));
            VE_PFX_READ(emitter.RateStart,
                        reader.NonNegative(*rate, "start", field + ".spawn.rate.start", 0.0f));
            VE_PFX_READ(
                emitter.RateDuration,
                reader.NonNegative(*rate, "duration", field + ".spawn.rate.duration", 0.0f));
            if (emitter.BurstCount == 0 && emitter.RatePerSecond <= 0.0f)
            {
                return std::unexpected(reader.Error(
                    field + ".spawn", "the emitter spawns nothing (no burst count and no rate)"));
            }

            vec2 range{};
            VE_PFX_READ(range,
                        reader.Range(source, "lifetime", field + ".lifetime", vec2(1.0f), true));
            emitter.Lifetime[0] = range.x;
            emitter.Lifetime[1] = range.y;

            const json* shape = nullptr;
            VE_PFX_READ(shape, reader.Object(source, "shape", field + ".shape"));
            VE_PFX_READ(emitter.Shape, reader.Enum(*shape, "kind", field + ".shape.kind",
                                                   {"point", "sphere", "shell"}, 0));
            VE_PFX_READ(emitter.ShapeRadius,
                        reader.NonNegative(*shape, "radius", field + ".shape.radius", 0.0f));

            const json* velocity = nullptr;
            VE_PFX_READ(velocity, reader.Object(source, "velocity", field + ".velocity"));
            VE_PFX_READ(range, reader.Range(*velocity, "speed", field + ".velocity.speed",
                                            vec2(0.0f), false));
            emitter.Speed[0] = range.x;
            emitter.Speed[1] = range.y;
            const json* cone = nullptr;
            VE_PFX_READ(cone, reader.Object(*velocity, "cone", field + ".velocity.cone"));
            vec3 triple{};
            VE_PFX_READ(triple, reader.Triple(*cone, "axis", field + ".velocity.cone.axis",
                                              vec3(0.0f, 1.0f, 0.0f)));
            if (triple == vec3(0.0f))
            {
                return std::unexpected(
                    reader.Error(field + ".velocity.cone.axis", "must not be the zero vector"));
            }
            std::memcpy(emitter.ConeAxis, &triple, sizeof(emitter.ConeAxis));
            VE_PFX_READ(emitter.ConeAngle,
                        reader.Number(*cone, "angle", field + ".velocity.cone.angle", 180.0f));
            if (!(emitter.ConeAngle > 0.0f && emitter.ConeAngle <= 180.0f))
            {
                return std::unexpected(
                    reader.Error(field + ".velocity.cone.angle", "must lie in (0, 180]"));
            }
            VE_PFX_READ(
                emitter.SpeedByRadius,
                reader.Number(*velocity, "speedByRadius", field + ".velocity.speedByRadius", 0.0f));
            const json* noise = nullptr;
            VE_PFX_READ(noise, reader.Object(*velocity, "noise", field + ".velocity.noise"));
            VE_PFX_READ(
                emitter.NoiseAmplitude,
                reader.NonNegative(*noise, "amplitude", field + ".velocity.noise.amplitude", 0.0f));
            VE_PFX_READ(
                emitter.NoiseFrequency,
                reader.NonNegative(*noise, "frequency", field + ".velocity.noise.frequency", 1.0f));

            VE_PFX_READ(triple,
                        reader.Triple(source, "acceleration", field + ".acceleration", vec3(0.0f)));
            std::memcpy(emitter.Acceleration, &triple, sizeof(emitter.Acceleration));
            VE_PFX_READ(emitter.Drag, reader.NonNegative(source, "drag", field + ".drag", 0.0f));

            VE_PFX_READ(range, reader.Range(source, "size", field + ".size", vec2(1.0f), true));
            emitter.Size[0] = range.x;
            emitter.Size[1] = range.y;
            VE_PFX_READ(range,
                        reader.Range(source, "rotation", field + ".rotation", vec2(0.0f), false));
            emitter.Rotation[0] = range.x;
            emitter.Rotation[1] = range.y;
            VE_PFX_READ(range, reader.Range(source, "spin", field + ".spin", vec2(0.0f), false));
            emitter.Spin[0] = range.x;
            emitter.Spin[1] = range.y;

            const json* playback = nullptr;
            VE_PFX_READ(playback, reader.Object(source, "playback", field + ".playback"));
            VE_PFX_READ(
                emitter.PlaybackMode,
                reader.Enum(*playback, "mode", field + ".playback.mode", {"lifetime", "fps"}, 0));
            VE_PFX_READ(emitter.PlaybackFps,
                        reader.NonNegative(*playback, "fps", field + ".playback.fps", 0.0f));
            if (playback->contains("randomStart") && !(*playback)["randomStart"].is_null())
            {
                if (!(*playback)["randomStart"].is_boolean())
                {
                    return std::unexpected(
                        reader.Error(field + ".playback.randomStart", "must be a boolean"));
                }
                emitter.RandomStart = (*playback)["randomStart"].get<bool>() ? 1u : 0u;
            }

            if (source.contains("softDistance") && !source["softDistance"].is_null())
            {
                VE_PFX_READ(
                    emitter.SoftDistance,
                    reader.NonNegative(source, "softDistance", field + ".softDistance", 0.0f));
                emitter.HasSoftDistance = 1;
            }

            if (source.contains("emission") && !source["emission"].is_null())
            {
                if (!billboard)
                {
                    return std::unexpected(reader.Error(field + ".emission",
                                                        "a streak has no temperature to emit by"));
                }
                const json* emission = nullptr;
                VE_PFX_READ(emission, reader.Object(source, "emission", field + ".emission"));
                VE_PFX_READ(emitter.EmissionRamp,
                            ReadCurve(reader, *emission, "ramp", field + ".emission.ramp", true,
                                      MaxFlipbookEmissionKeys, pool));
                VE_PFX_READ(emitter.Ignition, reader.Number(*emission, "ignition",
                                                            field + ".emission.ignition", 0.0f));
                emitter.HasEmission = 1;
            }

            const json* streak = nullptr;
            VE_PFX_READ(streak, reader.Object(source, "streak", field + ".streak"));
            VE_PFX_READ(triple,
                        reader.Triple(*streak, "color", field + ".streak.color", vec3(1.0f)));
            std::memcpy(emitter.StreakColor, &triple, sizeof(emitter.StreakColor));
            VE_PFX_READ(
                emitter.StreakLengthScale,
                reader.NonNegative(*streak, "lengthScale", field + ".streak.lengthScale", 0.05f));

            const json* overLife = nullptr;
            VE_PFX_READ(overLife, reader.Object(source, "overLife", field + ".overLife"));
            VE_PFX_READ(emitter.SizeCurve,
                        ReadCurve(reader, *overLife, "size", field + ".overLife.size", false,
                                  MaxParticleCurveKeys, pool));
            VE_PFX_READ(emitter.OpacityCurve,
                        ReadCurve(reader, *overLife, "opacity", field + ".overLife.opacity", false,
                                  MaxParticleCurveKeys, pool));
            VE_PFX_READ(emitter.TintCurve,
                        ReadCurve(reader, *overLife, "tint", field + ".overLife.tint", true,
                                  MaxParticleCurveKeys, pool));
            VE_PFX_READ(emitter.IntensityCurve,
                        ReadCurve(reader, *overLife, "intensity", field + ".overLife.intensity",
                                  false, MaxParticleCurveKeys, pool));
            VE_PFX_READ(emitter.TemperatureCurve,
                        ReadCurve(reader, *overLife, "temperature", field + ".overLife.temperature",
                                  false, MaxParticleCurveKeys, pool));
            return emitter;
        }

        Result<CookedParticleFlash> ReadFlash(const Reader& reader, const json& source,
                                              const string& field, KeyPool& pool)
        {
            if (!source.is_object())
            {
                return std::unexpected(reader.Error(field, "must be an object"));
            }
            CookedParticleFlash flash;
            VE_PFX_READ(flash.Time, reader.NonNegative(source, "time", field + ".time", 0.0f));
            VE_PFX_READ(flash.Duration,
                        reader.NonNegative(source, "duration", field + ".duration", 0.2f));
            vec3 colour{};
            VE_PFX_READ(colour, reader.Triple(source, "color", field + ".color", vec3(1.0f)));
            std::memcpy(flash.Color, &colour, sizeof(flash.Color));
            VE_PFX_READ(flash.Intensity,
                        reader.NonNegative(source, "intensity", field + ".intensity", 0.0f));
            VE_PFX_READ(flash.Range, reader.NonNegative(source, "range", field + ".range", 0.0f));
            VE_PFX_READ(flash.Radius,
                        reader.NonNegative(source, "radius", field + ".radius", 0.0f));
            VE_PFX_READ(flash.Curve, ReadCurve(reader, source, "curve", field + ".curve", false,
                                               MaxParticleCurveKeys, pool));
            return flash;
        }

#undef VE_PFX_READ

        template <class T>
        void Append(vector<u8>& blob, const T& value)
        {
            const usize offset = blob.size();
            blob.resize(offset + sizeof(T));
            std::memcpy(blob.data() + offset, &value, sizeof(T));
        }
    }

    Result<vector<u8>> ParticleEffectImporter::Cook(const CookContext& context,
                                                    const json& entry) const
    {
        if (!entry.contains("source") || !entry["source"].is_string())
        {
            return std::unexpected("particle effect importer: missing or invalid 'source'");
        }
        const path sourcePath = context.PackDir / entry["source"].get<string>();
        const Reader reader(sourcePath.string());

        const Result<json> document = ReadJsonFile(sourcePath, "particle effect importer");
        if (!document)
        {
            return std::unexpected(document.error());
        }
        const json& effect = *document;
        if (!effect.is_object() || !effect.contains("type") || !effect["type"].is_string() ||
            effect["type"].get<string>() != SchemaType)
        {
            return std::unexpected(reader.Error("type", fmt::format("must be \"{}\"", SchemaType)));
        }
        if (!effect.contains("version") || !effect["version"].is_number_integer() ||
            effect["version"].get<i64>() != SchemaVersion)
        {
            return std::unexpected(reader.Error(
                "version", fmt::format("unsupported version (expected {})", SchemaVersion)));
        }
        if (!effect.contains("emitters") || !effect["emitters"].is_array())
        {
            return std::unexpected(reader.Error("emitters", "must be an array"));
        }
        const json& emitters = effect["emitters"];
        if (emitters.size() > MaxParticleEmitters)
        {
            return std::unexpected(
                reader.Error("emitters", fmt::format("{} emitters exceed the {} an effect holds",
                                                     emitters.size(), MaxParticleEmitters)));
        }
        const json empty = json::array();
        const json& flashes =
            effect.contains("flashes") && !effect["flashes"].is_null() ? effect["flashes"] : empty;
        if (!flashes.is_array())
        {
            return std::unexpected(reader.Error("flashes", "must be an array"));
        }
        if (flashes.size() > MaxParticleFlashes)
        {
            return std::unexpected(
                reader.Error("flashes", fmt::format("{} flashes exceed the {} an effect holds",
                                                    flashes.size(), MaxParticleFlashes)));
        }

        KeyPool pool;
        vector<CookedParticleEmitter> cookedEmitters;
        u64 maxAlive = 0;
        for (usize e = 0; e < emitters.size(); ++e)
        {
            Result<CookedParticleEmitter> emitter =
                ReadEmitter(context, reader, emitters[e], fmt::format("emitters[{}]", e), pool);
            if (!emitter)
            {
                return std::unexpected(emitter.error());
            }
            maxAlive += ParticleEmitterMaxAlive(emitter->BurstCount, emitter->RatePerSecond,
                                                emitter->Lifetime[1]);
            cookedEmitters.push_back(*emitter);
        }
        vector<CookedParticleFlash> cookedFlashes;
        for (usize f = 0; f < flashes.size(); ++f)
        {
            Result<CookedParticleFlash> flash =
                ReadFlash(reader, flashes[f], fmt::format("flashes[{}]", f), pool);
            if (!flash)
            {
                return std::unexpected(flash.error());
            }
            cookedFlashes.push_back(*flash);
        }

        if (maxAlive > Renderer::DefaultParticleCapacity)
        {
            fmt::print(stderr,
                       "particle effect importer: '{}': up to {} particles can be alive at once, "
                       "more than a renderer's default capacity of {}; the excess is dropped\n",
                       sourcePath.string(), maxAlive, Renderer::DefaultParticleCapacity);
        }

        const CookedParticleEffectHeader header{
            .Version = CookedParticleEffectVersion,
            .EmitterCount = static_cast<u32>(cookedEmitters.size()),
            .FlashCount = static_cast<u32>(cookedFlashes.size()),
            .KeyFloats = static_cast<u32>(pool.Floats.size()),
        };
        vector<u8> blob;
        blob.reserve(sizeof(header) + cookedEmitters.size() * sizeof(CookedParticleEmitter) +
                     cookedFlashes.size() * sizeof(CookedParticleFlash) +
                     pool.Floats.size() * sizeof(f32));
        Append(blob, header);
        for (const CookedParticleEmitter& emitter : cookedEmitters)
        {
            Append(blob, emitter);
        }
        for (const CookedParticleFlash& flash : cookedFlashes)
        {
            Append(blob, flash);
        }
        for (const f32 key : pool.Floats)
        {
            Append(blob, key);
        }
        return blob;
    }
}
