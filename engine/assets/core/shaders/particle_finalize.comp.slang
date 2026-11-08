// Particle Finalize: one thread writes the alpha and additive draws' vertex counts and copies the
// counters into this frame's stats region, which the CPU reads when its slot comes round again —
// among them the survivors that may draw alpha, which bound the sort of the frames after.
#include "particle_compute.slang"

[shader("compute")]
[numthreads(1, 1, 1)]
void csMain()
{
    uint alpha = g_Set3.Counters.Load(ParticleCounterAlpha);
    uint additive = g_Set3.Counters.Load(ParticleCounterAdditive);
    g_Set3.Args.Store4(ParticleArgsAlphaDraw, uint4(alpha * 6u, 1u, 0u, 0u));
    g_Set3.Args.Store4(ParticleArgsAdditiveDraw, uint4(additive * 6u, 1u, 0u, 0u));
    g_Set3.Stats[0] = g_Set3.Counters.Load(LiveCounter(1u - g_PC.Cur));
    g_Set3.Stats[1] = g_Set3.Counters.Load(ParticleCounterSpawned);
    g_Set3.Stats[2] = g_Set3.Counters.Load(ParticleCounterDropped);
    g_Set3.Stats[3] = alpha;
    g_Set3.Stats[4] = g_PC.SortSize;
    g_Set3.Stats[5] = additive;
    g_Set3.Stats[6] = 1u;
    g_Set3.Stats[7] = g_Set3.Counters.Load(ParticleCounterAlphaLive);
}
