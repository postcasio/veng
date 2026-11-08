// The particle chain's compute stages' bindings and push block: every set-3 buffer, read-write
// where a stage writes it, and the constants the CPU pushes per dispatch.

#pragma once

#include "particle_types.slang"
#include "Veng/binding.slang"

struct ParticleComputeSet3
{
    RWStructuredBuffer<GpuParticle> Particles;
    RWStructuredBuffer<uint> DeadList;
    RWStructuredBuffer<uint> LiveList;
    RWStructuredBuffer<uint> AdditiveList;
    RWStructuredBuffer<uint2> AlphaKeys;
    RWStructuredBuffer<GpuParticleDraw> Draws;
    RWByteAddressBuffer Counters;
    RWByteAddressBuffer Args;
    StructuredBuffer<GpuParticleInstance> Instances;
    StructuredBuffer<GpuParticleEmitter> Emitters;
    StructuredBuffer<GpuParticleSpawnRange> SpawnRanges;
    RWStructuredBuffer<uint> Stats;
    StructuredBuffer<float> FrameScales;
    StructuredBuffer<GpuParticleFedSpawn> FedSpawns;
};
VE_DESCRIPTOR_SET(3, ParticleComputeSet3, g_Set3);

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

VE_PUSH_CONSTANTS(ParticleComputePush, g_PC);

// The byte offset of a live-list half's counter.
uint LiveCounter(uint half)
{
    return ParticleCounterLive0 + 4u * half;
}
