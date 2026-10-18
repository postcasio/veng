// The particle math, the shader side of the renderer's device-free ParticleMath.h and written line
// for line alike: the PCG hash and the random streams a particle's key roots, the hashed value
// noise, the spawn state an index derives, and the closed-form trajectory under constant
// acceleration and linear drag. Everything is 32-bit, so no stage needs int64.

#pragma once

// The PCG hash.
uint ParticleHash(uint v)
{
    uint s = v * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}

// A particle's key: every random draw it makes roots here.
uint ParticleKey(uint seed, uint emitter, uint index)
{
    return ParticleHash(seed ^ ParticleHash(emitter * 0x10000u + index));
}

// Draw k of a key's stream, uniform in [0, 1).
float ParticleRandom(uint key, uint k)
{
    return float(ParticleHash(key + k * 0x9E3779B9u) >> 8u) * (1.0 / 16777216.0);
}

// The value-noise lattice: a hash of an integer cell, in [-1, 1].
float ParticleLattice(int3 cell, uint seed)
{
    uint h = ParticleHash(seed ^ ParticleHash(uint(cell.x) +
                                              ParticleHash(uint(cell.y) + ParticleHash(uint(cell.z)))));
    return float(h >> 8u) * (2.0 / 16777216.0) - 1.0;
}

// Hashed-lattice trilinear value noise with a smoothstep fade, in [-1, 1].
float ParticleValueNoise(float3 p, uint seed)
{
    float3 cell = floor(p);
    float3 f = p - cell;
    float3 w = f * f * (3.0 - 2.0 * f);
    int3 c = int3(cell);
    float n000 = ParticleLattice(c + int3(0, 0, 0), seed);
    float n100 = ParticleLattice(c + int3(1, 0, 0), seed);
    float n010 = ParticleLattice(c + int3(0, 1, 0), seed);
    float n110 = ParticleLattice(c + int3(1, 1, 0), seed);
    float n001 = ParticleLattice(c + int3(0, 0, 1), seed);
    float n101 = ParticleLattice(c + int3(1, 0, 1), seed);
    float n011 = ParticleLattice(c + int3(0, 1, 1), seed);
    float n111 = ParticleLattice(c + int3(1, 1, 1), seed);
    float x00 = lerp(n000, n100, w.x);
    float x10 = lerp(n010, n110, w.x);
    float x01 = lerp(n001, n101, w.x);
    float x11 = lerp(n011, n111, w.x);
    return lerp(lerp(x00, x10, w.y), lerp(x01, x11, w.y), w.z);
}

// Rotates v by the unit quaternion q (x, y, z, w).
float3 ParticleRotate(float4 q, float3 v)
{
    float3 u = q.xyz;
    return v + 2.0 * cross(u, cross(u, v) + q.w * v);
}

// The random draws a particle's spawn takes, by stream position.
static const uint ParticleDrawDirectionZ = 0;
static const uint ParticleDrawDirectionPhi = 1;
static const uint ParticleDrawRadius = 2;
static const uint ParticleDrawSpeed = 3;
static const uint ParticleDrawLifetime = 4;
static const uint ParticleDrawSpin = 5;
static const uint ParticleDrawRotation = 6;
static const uint ParticleDrawSize = 7;
static const uint ParticleDrawStartFrame = 8;
static const uint ParticleDrawVariant = 9;

// Which of an emitter's flipbooks a particle plays, uniform over them; 0 and 1 both pick 0.
uint ParticleVariantOf(uint key, uint count)
{
    if (count <= 1u)
    {
        return 0u;
    }
    uint pick = uint(ParticleRandom(key, ParticleDrawVariant) * float(count));
    return min(pick, count - 1u);
}

// What a particle's spawn reads of its emitter.
struct ParticleSpawnParams
{
    uint Shape;          // 0 point, 1 sphere, 2 shell
    float Radius;        // the shape's radius R
    float2 Speed;        // the speed range
    float SpeedByRadius; // the exponent of (r / R)
    float3 ConeAxis;     // unit, in the effect's frame
    float ConeCos;       // cos of the cone's half-angle
    float NoiseAmplitude;
    float NoiseFrequency;
    float2 Lifetime;
    float2 Spin;         // radians per second
};

// Where a world-space instance's emitter stood at a particle's spawn time: its offset from the
// anchor, linear across the update spanning the time and held at either end outside it; zero for an
// instance that is not world-space.
float3 ParticleEmitterAt(float3 previous, float3 current, float previousTime, float time,
                         float spawnTime)
{
    float span = time - previousTime;
    float t = span > 0.0 ? clamp((spawnTime - previousTime) / span, 0.0, 1.0) : 1.0;
    return previous + (current - previous) * t;
}

// What a particle's spawn reads of its instance.
struct ParticleSpawnInstance
{
    float4 Orientation;
    float3 InheritVelocity;
    uint Seed;
    float Scale;
};

// A particle's state at spawn, relative to its instance's anchor.
struct ParticleSpawnState
{
    float3 Offset;
    float3 Velocity;
    float Lifetime;
    uint Random;
    float Spin;
};

// The spawn state of particle `index` of emitter `emitter`: a direction uniform over the cone about
// the oriented axis (the whole sphere at 180 degrees), a radius by the shape, and the speed
// lerp(speed) * (r / R)^speedByRadius * max(0, 1 + amplitude * N(d * frequency)).
ParticleSpawnState MakeParticleSpawnState(ParticleSpawnParams params, ParticleSpawnInstance instance,
                                          uint emitter, uint index)
{
    uint key = ParticleKey(instance.Seed, emitter, index);

    float3 n = ParticleRotate(instance.Orientation, params.ConeAxis);
    float sign = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (sign + n.z);
    float b = n.x * n.y * a;
    float3 b1 = float3(1.0 + sign * n.x * n.x * a, sign * b, -sign * n.x);
    float3 b2 = float3(b, sign + n.y * n.y * a, -n.y);
    float z = 1.0 - ParticleRandom(key, ParticleDrawDirectionZ) * (1.0 - params.ConeCos);
    float ring = sqrt(max(0.0, 1.0 - z * z));
    float phi = 6.28318530717958647692 * ParticleRandom(key, ParticleDrawDirectionPhi);
    float3 d = b1 * (ring * cos(phi)) + b2 * (ring * sin(phi)) + n * z;

    float r = 0.0;
    if (params.Shape == 1u)
    {
        r = params.Radius * pow(ParticleRandom(key, ParticleDrawRadius), 1.0 / 3.0);
    }
    else if (params.Shape == 2u)
    {
        r = params.Radius;
    }
    float radiusFactor = params.Radius > 0.0 && params.Shape != 0u
                             ? pow(r / params.Radius, params.SpeedByRadius)
                             : 1.0;
    float noise = params.NoiseAmplitude > 0.0
                      ? ParticleValueNoise(d * params.NoiseFrequency, ParticleHash(instance.Seed + emitter))
                      : 0.0;
    float speed = lerp(params.Speed.x, params.Speed.y, ParticleRandom(key, ParticleDrawSpeed)) *
                  radiusFactor * max(0.0, 1.0 + params.NoiseAmplitude * noise);

    ParticleSpawnState state;
    state.Offset = d * (r * instance.Scale);
    state.Velocity = d * (speed * instance.Scale) + instance.InheritVelocity;
    state.Lifetime =
        lerp(params.Lifetime.x, params.Lifetime.y, ParticleRandom(key, ParticleDrawLifetime));
    state.Random = key;
    state.Spin = lerp(params.Spin.x, params.Spin.y, ParticleRandom(key, ParticleDrawSpin));
    return state;
}

// Below this k * t the drag terms take their series, so the two branches meet smoothly.
static const float ParticleDragSeriesLimit = 1e-3;

// F = (1 - e^(-k t)) / k, the distance a unit velocity covers under drag k by time t.
float ParticleDragDistance(float k, float t)
{
    float x = k * t;
    if (x < ParticleDragSeriesLimit)
    {
        return t * (1.0 - x * (1.0 / 2.0) + x * x * (1.0 / 6.0) - x * x * x * (1.0 / 24.0));
    }
    return (1.0 - exp(-x)) / k;
}

// (t - F) / k, the distance a unit acceleration covers under drag k by time t.
float ParticleDragAccelDistance(float k, float t)
{
    float x = k * t;
    if (x < ParticleDragSeriesLimit)
    {
        return t * t * (1.0 / 2.0 - x * (1.0 / 6.0) + x * x * (1.0 / 24.0) - x * x * x * (1.0 / 120.0));
    }
    return (t - ParticleDragDistance(k, t)) / k;
}

// The offset at an age under constant acceleration a and linear drag k: p0 + v0 t + a t^2 / 2 with
// no drag, p0 + v0 F + a (t - F) / k with it. With a = 0 and k = 0 it is exactly p0 + v0 t.
float3 ParticleOffsetAt(float3 p0, float3 v0, float3 a, float k, float t)
{
    if (k == 0.0)
    {
        return p0 + v0 * t + a * (0.5 * t * t);
    }
    return p0 + v0 * ParticleDragDistance(k, t) + a * ParticleDragAccelDistance(k, t);
}

// The velocity at an age: v0 + a t with no drag, v0 e^(-k t) + a F with it.
float3 ParticleVelocityAt(float3 v0, float3 a, float k, float t)
{
    if (k == 0.0)
    {
        return v0 + a * t;
    }
    return v0 * exp(-k * t) + a * ParticleDragDistance(k, t);
}
