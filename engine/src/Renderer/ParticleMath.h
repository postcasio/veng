#pragma once

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>

#include <Veng/Veng.h>
#include <Veng/Asset/ParticleEffect.h>

/// @brief The particle math, device-free: the C++ twin of `particle_math.slang`.
///
/// Written line for line alike with the shader, as InstancedCull.h mirrors its shader, so the unit
/// band pins what the GPU computes: the PCG hash and a particle's random streams, the hashed value
/// noise, the spawn state an index derives, and the closed-form trajectory under constant
/// acceleration and linear drag. Everything is 32-bit.
namespace Veng::Renderer
{
    /// @brief The PCG hash.
    [[nodiscard]] inline u32 ParticleHash(const u32 v)
    {
        const u32 s = v * 747796405u + 2891336453u;
        const u32 w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
        return (w >> 22u) ^ w;
    }

    /// @brief A particle's key, which every random draw it makes roots in.
    [[nodiscard]] inline u32 ParticleKey(const u32 seed, const u32 emitter, const u32 index)
    {
        return ParticleHash(seed ^ ParticleHash(emitter * 0x10000u + index));
    }

    /// @brief Draw k of a key's stream, uniform in [0, 1).
    [[nodiscard]] inline f32 ParticleRandom(const u32 key, const u32 k)
    {
        return static_cast<f32>(ParticleHash(key + k * 0x9E3779B9u) >> 8u) * (1.0f / 16777216.0f);
    }

    /// @brief The value-noise lattice: a hash of an integer cell, in [-1, 1].
    [[nodiscard]] inline f32 ParticleLattice(const ivec3 cell, const u32 seed)
    {
        const u32 h =
            ParticleHash(seed ^ ParticleHash(static_cast<u32>(cell.x) +
                                             ParticleHash(static_cast<u32>(cell.y) +
                                                          ParticleHash(static_cast<u32>(cell.z)))));
        return static_cast<f32>(h >> 8u) * (2.0f / 16777216.0f) - 1.0f;
    }

    /// @brief Hashed-lattice trilinear value noise with a smoothstep fade, in [-1, 1].
    [[nodiscard]] inline f32 ParticleValueNoise(const vec3 p, const u32 seed)
    {
        const vec3 cell = glm::floor(p);
        const vec3 f = p - cell;
        const vec3 w = f * f * (3.0f - 2.0f * f);
        const ivec3 c(cell);
        const auto at = [&](const i32 x, const i32 y, const i32 z)
        { return ParticleLattice(c + ivec3(x, y, z), seed); };
        const f32 x00 = glm::mix(at(0, 0, 0), at(1, 0, 0), w.x);
        const f32 x10 = glm::mix(at(0, 1, 0), at(1, 1, 0), w.x);
        const f32 x01 = glm::mix(at(0, 0, 1), at(1, 0, 1), w.x);
        const f32 x11 = glm::mix(at(0, 1, 1), at(1, 1, 1), w.x);
        return glm::mix(glm::mix(x00, x10, w.y), glm::mix(x01, x11, w.y), w.z);
    }

    /// @brief Rotates v by the unit quaternion q, stored (x, y, z, w).
    [[nodiscard]] inline vec3 ParticleRotate(const vec4 q, const vec3 v)
    {
        const vec3 u(q);
        return v + 2.0f * glm::cross(u, glm::cross(u, v) + q.w * v);
    }

    /// @brief Positions of the draws a particle's key stream takes.
    namespace ParticleDraw
    {
        /// @brief The direction's cosine to the cone's axis.
        inline constexpr u32 DirectionZ = 0;
        /// @brief The direction's angle about the cone's axis.
        inline constexpr u32 DirectionPhi = 1;
        /// @brief The radius within a sphere.
        inline constexpr u32 Radius = 2;
        /// @brief The speed within its range.
        inline constexpr u32 Speed = 3;
        /// @brief The lifetime within its range.
        inline constexpr u32 Lifetime = 4;
        /// @brief The spin within its range.
        inline constexpr u32 Spin = 5;
        /// @brief The initial roll within its range.
        inline constexpr u32 Rotation = 6;
        /// @brief The size within its range.
        inline constexpr u32 Size = 7;
        /// @brief The random start frame.
        inline constexpr u32 StartFrame = 8;
        /// @brief The flipbook variant.
        inline constexpr u32 Variant = 9;
    }

    /// @brief Returns which of an emitter's flipbooks a particle plays, uniform over them.
    /// @param key    The particle's key (ParticleSpawnState::Random).
    /// @param count  How many flipbooks the emitter picks from; 0 and 1 both pick 0.
    /// @return The variant, in [0, max(count, 1)).
    [[nodiscard]] inline u32 ParticleVariantOf(const u32 key, const u32 count)
    {
        if (count <= 1u)
        {
            return 0u;
        }
        const auto pick =
            static_cast<u32>(ParticleRandom(key, ParticleDraw::Variant) * static_cast<f32>(count));
        return std::min(pick, count - 1u);
    }

    /// @brief What a particle's spawn reads of its emitter.
    struct ParticleSpawnParams
    {
        /// @brief The ParticleShape integer.
        u32 Shape = 0;
        /// @brief The shape's radius R.
        f32 Radius = 0.0f;
        /// @brief The speed range.
        vec2 Speed{0.0f};
        /// @brief The exponent of (r / R).
        f32 SpeedByRadius = 0.0f;
        /// @brief The cone's unit axis in the effect's frame.
        vec3 ConeAxis{0.0f, 1.0f, 0.0f};
        /// @brief The cosine of the cone's half-angle.
        f32 ConeCos = -1.0f;
        /// @brief The speed noise's relative amplitude.
        f32 NoiseAmplitude = 0.0f;
        /// @brief The speed noise's spatial frequency.
        f32 NoiseFrequency = 1.0f;
        /// @brief The lifetime range.
        vec2 Lifetime{1.0f};
        /// @brief The spin range, radians per second.
        vec2 Spin{0.0f};
    };

    /// @brief What a particle's spawn reads of its instance.
    struct ParticleSpawnInstance
    {
        /// @brief The effect's frame, a quaternion stored (x, y, z, w).
        vec4 Orientation{0.0f, 0.0f, 0.0f, 1.0f};
        /// @brief Added to every initial velocity.
        vec3 InheritVelocity{0.0f};
        /// @brief The instance's seed.
        u32 Seed = 0;
        /// @brief Multiplies every authored length.
        f32 Scale = 1.0f;
    };

    /// @brief A particle's state at spawn, relative to its instance's anchor.
    struct ParticleSpawnState
    {
        /// @brief The offset from the anchor.
        vec3 Offset{0.0f};
        /// @brief The initial velocity, inherited velocity included.
        vec3 Velocity{0.0f};
        /// @brief Seconds the particle lives.
        f32 Lifetime = 0.0f;
        /// @brief The particle's key.
        u32 Random = 0;
        /// @brief The roll rate, radians per second.
        f32 Spin = 0.0f;
    };

    /// @brief Reads an emitter's spawn parameters out of its description.
    /// @param emitter  The emitter.
    /// @return The spawn parameters, angles in radians and the cone axis normalised.
    [[nodiscard]] inline ParticleSpawnParams
    MakeParticleSpawnParams(const ParticleEmitterDesc& emitter)
    {
        const f32 axisLength = glm::length(emitter.ConeAxis);
        return {
            .Shape = static_cast<u32>(emitter.Shape),
            .Radius = emitter.ShapeRadius,
            .Speed = emitter.Speed,
            .SpeedByRadius = emitter.SpeedByRadius,
            .ConeAxis = axisLength > 0.0f ? emitter.ConeAxis / axisLength : vec3(0.0f, 1.0f, 0.0f),
            .ConeCos = std::cos(glm::radians(std::clamp(emitter.ConeAngle, 0.0f, 180.0f))),
            .NoiseAmplitude = emitter.NoiseAmplitude,
            .NoiseFrequency = emitter.NoiseFrequency,
            .Lifetime = emitter.Lifetime,
            .Spin = glm::radians(emitter.Spin),
        };
    }

    /// @brief The spawn state of particle @p index of emitter @p emitter.
    ///
    /// A direction uniform over the cone about the oriented axis (the whole sphere at 180 degrees),
    /// placed at the shape's radius along it, moving at lerp(speed) * (r / R)^speedByRadius *
    /// max(0, 1 + amplitude * N(d * frequency)), scaled by the instance and with its inherited
    /// velocity added.
    [[nodiscard]] inline ParticleSpawnState
    MakeParticleSpawnState(const ParticleSpawnParams& params, const ParticleSpawnInstance& instance,
                           const u32 emitter, const u32 index)
    {
        const u32 key = ParticleKey(instance.Seed, emitter, index);

        const vec3 n = ParticleRotate(instance.Orientation, params.ConeAxis);
        const f32 sign = n.z >= 0.0f ? 1.0f : -1.0f;
        const f32 a = -1.0f / (sign + n.z);
        const f32 b = n.x * n.y * a;
        const vec3 b1(1.0f + sign * n.x * n.x * a, sign * b, -sign * n.x);
        const vec3 b2(b, sign + n.y * n.y * a, -n.y);
        const f32 z =
            1.0f - ParticleRandom(key, ParticleDraw::DirectionZ) * (1.0f - params.ConeCos);
        const f32 ring = std::sqrt(std::max(0.0f, 1.0f - z * z));
        const f32 phi = 6.28318530717958647692f * ParticleRandom(key, ParticleDraw::DirectionPhi);
        const vec3 d = b1 * (ring * std::cos(phi)) + b2 * (ring * std::sin(phi)) + n * z;

        f32 r = 0.0f;
        if (params.Shape == 1u)
        {
            r = params.Radius * std::pow(ParticleRandom(key, ParticleDraw::Radius), 1.0f / 3.0f);
        }
        else if (params.Shape == 2u)
        {
            r = params.Radius;
        }
        const f32 radiusFactor = params.Radius > 0.0f && params.Shape != 0u
                                     ? std::pow(r / params.Radius, params.SpeedByRadius)
                                     : 1.0f;
        const f32 noise = params.NoiseAmplitude > 0.0f
                              ? ParticleValueNoise(d * params.NoiseFrequency,
                                                   ParticleHash(instance.Seed + emitter))
                              : 0.0f;
        const f32 speed =
            glm::mix(params.Speed.x, params.Speed.y, ParticleRandom(key, ParticleDraw::Speed)) *
            radiusFactor * std::max(0.0f, 1.0f + params.NoiseAmplitude * noise);

        return {
            .Offset = d * (r * instance.Scale),
            .Velocity = d * (speed * instance.Scale) + instance.InheritVelocity,
            .Lifetime = glm::mix(params.Lifetime.x, params.Lifetime.y,
                                 ParticleRandom(key, ParticleDraw::Lifetime)),
            .Random = key,
            .Spin = glm::mix(params.Spin.x, params.Spin.y, ParticleRandom(key, ParticleDraw::Spin)),
        };
    }

    /// @brief Below this k * t the drag terms take their series, so the two branches meet.
    inline constexpr f32 ParticleDragSeriesLimit = 1e-3f;

    /// @brief F = (1 - e^(-k t)) / k: how far a unit velocity carries under drag k by time t.
    [[nodiscard]] inline f32 ParticleDragDistance(const f32 k, const f32 t)
    {
        const f32 x = k * t;
        if (x < ParticleDragSeriesLimit)
        {
            return t *
                   (1.0f - x * (1.0f / 2.0f) + x * x * (1.0f / 6.0f) - x * x * x * (1.0f / 24.0f));
        }
        return (1.0f - std::exp(-x)) / k;
    }

    /// @brief (t - F) / k: how far a unit acceleration carries under drag k by time t.
    [[nodiscard]] inline f32 ParticleDragAccelDistance(const f32 k, const f32 t)
    {
        const f32 x = k * t;
        if (x < ParticleDragSeriesLimit)
        {
            return t * t *
                   (1.0f / 2.0f - x * (1.0f / 6.0f) + x * x * (1.0f / 24.0f) -
                    x * x * x * (1.0f / 120.0f));
        }
        return (t - ParticleDragDistance(k, t)) / k;
    }

    /// @brief The offset at an age under constant acceleration a and linear drag k.
    ///
    /// p0 + v0 t + a t^2 / 2 with no drag; p0 + v0 F + a (t - F) / k with it. With a = 0 and k = 0
    /// it is exactly p0 + v0 t.
    [[nodiscard]] inline vec3 ParticleOffsetAt(const vec3 p0, const vec3 v0, const vec3 a,
                                               const f32 k, const f32 t)
    {
        if (k == 0.0f)
        {
            return p0 + v0 * t + a * (0.5f * t * t);
        }
        return p0 + v0 * ParticleDragDistance(k, t) + a * ParticleDragAccelDistance(k, t);
    }

    /// @brief The velocity at an age: v0 + a t with no drag, v0 e^(-k t) + a F with it.
    [[nodiscard]] inline vec3 ParticleVelocityAt(const vec3 v0, const vec3 a, const f32 k,
                                                 const f32 t)
    {
        if (k == 0.0f)
        {
            return v0 + a * t;
        }
        return v0 * std::exp(-k * t) + a * ParticleDragDistance(k, t);
    }
}
