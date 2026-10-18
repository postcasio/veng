// The particle draw's bindings, push block and the interface between its vertex and fragment
// stages. The graphics stages read the chain's buffers read-only.

#pragma once

#include "particle_types.slang"

[[vk::binding(PARTICLE_BINDING_ADDITIVE_LIST, 3)]] StructuredBuffer<uint> g_AdditiveList;
[[vk::binding(PARTICLE_BINDING_ALPHA_KEYS, 3)]] StructuredBuffer<uint2> g_AlphaKeys;
[[vk::binding(PARTICLE_BINDING_DRAWS, 3)]] StructuredBuffer<GpuParticleDraw> g_Draws;
[[vk::binding(PARTICLE_BINDING_EMITTERS, 3)]] StructuredBuffer<GpuParticleEmitter> g_Emitters;

// Matches ParticleScenePass's ParticleDrawPush.
struct ParticleDrawPush
{
    uint ViewConstantsIndex;
    uint List; // 0 the sorted alpha keys, 1 the additive list
    uint Pad0;
    uint Pad1;
};

[[vk::push_constant]] ConstantBuffer<ParticleDrawPush> g_PC;

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
