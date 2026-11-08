// The particle draw's bindings, push block and the interface between its vertex and fragment
// stages. The graphics stages read the chain's buffers read-only.

#pragma once

#include "particle_types.slang"
#include "Veng/binding.slang"

struct ParticleDrawSet3
{
    ByteAddressBuffer Particles; // read by the compute stages only
    ByteAddressBuffer DeadList; // read by the compute stages only
    ByteAddressBuffer LiveList; // read by the compute stages only
    StructuredBuffer<uint> AdditiveList;
    StructuredBuffer<uint2> AlphaKeys;
    StructuredBuffer<GpuParticleDraw> Draws;
    ByteAddressBuffer Counters; // read by the compute stages only
    ByteAddressBuffer Args; // read by the compute stages only
    ByteAddressBuffer Instances; // read by the compute stages only
    StructuredBuffer<GpuParticleEmitter> Emitters;
};
VE_DESCRIPTOR_SET(3, ParticleDrawSet3, g_Set3);

// Matches ParticleScenePass's ParticleDrawPush.
struct ParticleDrawPush
{
    uint ViewConstantsIndex;
    uint List; // 0 the sorted alpha keys, 1 the additive list
    uint Pad0;
    uint Pad1;
};

VE_PUSH_CONSTANTS(ParticleDrawPush, g_PC);

struct VSOutput
{
    float4 sv_position : SV_Position;
    // A billboard's frame UV (u right, v down), or a streak's (along, across in [-1, 1]).
    float2 v_Quad : TEXCOORD0;
    nointerpolation uint v_Draw : TEXCOORD1;
    // The positive view-space depth, exact under perspective-correct interpolation of a flat quad.
    float v_ViewDepth : TEXCOORD2;
    // The coverage a streak narrower than the pixel floor keeps; 1 otherwise.
    nointerpolation float v_Coverage : TEXCOORD3;
    // 1 for a streak drawn as a square about its head, whose v_Quad is then its disc coordinate.
    nointerpolation uint v_Round : TEXCOORD4;
};
