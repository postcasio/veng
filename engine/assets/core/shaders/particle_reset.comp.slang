// Particle Reset: fills a freshly allocated pool's dead list with every slot and zeroes its
// counters. Runs once, on the first frame after the pool is allocated.
#include "particle_compute.slang"

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x < g_PC.Capacity)
    {
        g_Set3.DeadList[id.x] = g_PC.Capacity - 1u - id.x;
    }
    if (id.x == 0u)
    {
        g_Set3.Counters.Store(ParticleCounterDead, g_PC.Capacity);
        g_Set3.Counters.Store(ParticleCounterLive0, 0u);
        g_Set3.Counters.Store(ParticleCounterLive0 + 4u, 0u);
        g_Set3.Counters.Store(ParticleCounterAlpha, 0u);
        g_Set3.Counters.Store(ParticleCounterAdditive, 0u);
        g_Set3.Counters.Store(ParticleCounterSpawned, 0u);
        g_Set3.Counters.Store(ParticleCounterDropped, 0u);
        g_Set3.Counters.Store(ParticleCounterAlphaLive, 0u);
    }
}
