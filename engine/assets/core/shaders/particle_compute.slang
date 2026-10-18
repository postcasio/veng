// The particle chain's compute stages' bindings and push block: every set-3 buffer, read-write
// where a stage writes it, and the constants the CPU pushes per dispatch.

#pragma once

#include "particle_types.slang"

[[vk::binding(PARTICLE_BINDING_PARTICLES, 3)]] RWStructuredBuffer<GpuParticle> g_Particles;
[[vk::binding(PARTICLE_BINDING_DEAD_LIST, 3)]] RWStructuredBuffer<uint> g_DeadList;
[[vk::binding(PARTICLE_BINDING_LIVE_LIST, 3)]] RWStructuredBuffer<uint> g_LiveList;
[[vk::binding(PARTICLE_BINDING_ADDITIVE_LIST, 3)]] RWStructuredBuffer<uint> g_AdditiveList;
[[vk::binding(PARTICLE_BINDING_ALPHA_KEYS, 3)]] RWStructuredBuffer<uint2> g_AlphaKeys;
[[vk::binding(PARTICLE_BINDING_DRAWS, 3)]] RWStructuredBuffer<GpuParticleDraw> g_Draws;
[[vk::binding(PARTICLE_BINDING_COUNTERS, 3)]] RWByteAddressBuffer g_Counters;
[[vk::binding(PARTICLE_BINDING_ARGS, 3)]] RWByteAddressBuffer g_Args;
[[vk::binding(PARTICLE_BINDING_INSTANCES, 3)]] StructuredBuffer<GpuParticleInstance> g_Instances;
[[vk::binding(PARTICLE_BINDING_EMITTERS, 3)]] StructuredBuffer<GpuParticleEmitter> g_Emitters;
[[vk::binding(PARTICLE_BINDING_SPAWN_RANGES, 3)]]
StructuredBuffer<GpuParticleSpawnRange> g_SpawnRanges;
[[vk::binding(PARTICLE_BINDING_STATS, 3)]] RWStructuredBuffer<uint> g_Stats;
[[vk::binding(PARTICLE_BINDING_FRAME_SCALES, 3)]] StructuredBuffer<float> g_FrameScales;
[[vk::binding(PARTICLE_BINDING_FED_SPAWNS, 3)]] StructuredBuffer<GpuParticleFedSpawn> g_FedSpawns;

// Matches ParticleSimulation's ParticleComputePush.
struct ParticleComputePush
{
    uint Cur;                // which half of the live list holds the frame's particles
    uint Capacity;           // the pool's capacity N
    uint SpawnTotal;         // the spawn dispatch's threads
    uint RangeCount;         // the spawn ranges
    uint SortStage;          // the bitonic stage's block size k
    uint SortStep;           // the compare distance j
    uint SortSize;           // the sorted length P
    uint ViewConstantsIndex; // the view block the update lights and sorts against
};

[[vk::push_constant]] ConstantBuffer<ParticleComputePush> g_PC;

// The byte offset of a live-list half's counter.
uint LiveCounter(uint half)
{
    return ParticleCounterLive0 + 4u * half;
}
