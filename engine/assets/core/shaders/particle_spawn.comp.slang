// Particle Spawn: one thread per index the frame's spawn ranges issue. A thread finds its range by
// a binary search over the ranges' prefixes, derives its particle's spawn state from the index
// alone, pops a free slot, writes the state and appends the slot to the live list. A particle
// already past its lifetime at this frame's time takes no slot and is not counted; a spawn finding
// no free slot is dropped and counted.
#include "particle_compute.slang"
#include "particle_math.slang"

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_PC.SpawnTotal)
    {
        return;
    }

    // The last range whose prefix is at most the thread.
    uint lo = 0u;
    uint hi = g_PC.RangeCount;
    while (hi - lo > 1u)
    {
        uint mid = (lo + hi) / 2u;
        if (g_SpawnRanges[mid].Prefix <= id.x)
        {
            lo = mid;
        }
        else
        {
            hi = mid;
        }
    }
    GpuParticleSpawnRange range = g_SpawnRanges[lo];
    uint index = range.FirstIndex + (id.x - range.Prefix);
    uint slot = range.InstanceEmitter & 0xFFFFu;
    uint emitterIndex = range.InstanceEmitter >> 16;
    GpuParticleInstance instance = g_Instances[slot];
    GpuParticleEmitter emitter = g_Emitters[instance.EmitterBase + emitterIndex];

    uint burst = emitter.Counts.x;
    float spawnTime = index < burst ? emitter.Spawn.x
                                    : emitter.Spawn.y + float(index - burst) / emitter.Spawn.z;

    ParticleSpawnParams params;
    params.Shape = emitter.Mode.z;
    params.Radius = emitter.Speed.w;
    params.Speed = emitter.Speed.xy;
    params.SpeedByRadius = emitter.Speed.z;
    params.ConeAxis = emitter.Cone.xyz;
    params.ConeCos = emitter.Cone.w;
    params.NoiseAmplitude = emitter.Noise.x;
    params.NoiseFrequency = emitter.Noise.y;
    params.Lifetime = emitter.LifetimeSize.xy;
    params.Spin = emitter.Rotation.zw;
    ParticleSpawnInstance spawner;
    spawner.Orientation = instance.Orientation;
    spawner.InheritVelocity = instance.InheritVelocity;
    spawner.Seed = instance.Seed;
    spawner.Scale = instance.Scale;
    ParticleSpawnState state = MakeParticleSpawnState(params, spawner, emitterIndex, index);
    if (instance.Time - spawnTime >= state.Lifetime)
    {
        return;
    }

    uint previous;
    g_Counters.InterlockedAdd(ParticleCounterDead, 0xFFFFFFFFu, previous);
    if (int(previous) <= 0)
    {
        uint ignored;
        g_Counters.InterlockedAdd(ParticleCounterDead, 1u, ignored);
        g_Counters.InterlockedAdd(ParticleCounterDropped, 1u, ignored);
        return;
    }
    uint particleSlot = g_DeadList[previous - 1u];

    GpuParticle particle;
    particle.Offset = state.Offset;
    particle.SpawnTime = spawnTime;
    particle.Velocity = state.Velocity;
    particle.Lifetime = state.Lifetime;
    particle.Instance = slot | ((instance.Generation & 0xFFFFu) << 16);
    particle.Emitter = emitterIndex;
    particle.Random = state.Random;
    particle.Spin = state.Spin;
    g_Particles[particleSlot] = particle;

    uint live;
    g_Counters.InterlockedAdd(LiveCounter(g_PC.Cur), 1u, live);
    g_LiveList[g_PC.Cur * g_PC.Capacity + live] = particleSlot;
    uint spawned;
    g_Counters.InterlockedAdd(ParticleCounterSpawned, 1u, spawned);
}
