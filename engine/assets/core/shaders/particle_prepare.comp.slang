// Particle Prepare: one thread writes the update's dispatch arguments from the live count and
// zeroes the counters the update appends through.
#include "particle_compute.slang"

[shader("compute")]
[numthreads(1, 1, 1)]
void csMain()
{
    uint live = g_Set3.Counters.Load(LiveCounter(g_PC.Cur));
    g_Set3.Args.Store3(ParticleArgsUpdate, uint3((live + 63u) / 64u, 1u, 1u));
    g_Set3.Counters.Store(LiveCounter(1u - g_PC.Cur), 0u);
    g_Set3.Counters.Store(ParticleCounterAlpha, 0u);
    g_Set3.Counters.Store(ParticleCounterAdditive, 0u);
    g_Set3.Counters.Store(ParticleCounterAlphaLive, 0u);
}
