#include "Veng/material.slang"

// The template's trivial surface fragment stage: it writes a flat, untextured
// metallic-roughness surface into the g-buffer straight from the authored
// MaterialParams scalars — no texture sampling, no normal map. BaseColorFactor is the
// albedo, the metallic/roughness factors are the packed ORM channels, and occlusion is
// fully open. It is the smallest fragment that drives the deferred lighting pass to a
// lit, solid-colour cube.

// This material's parameter block: the smallest metallic-roughness set the flat shader
// reads. Read through the engine's set-0 g_MaterialParams at index * MaterialParamStride.
struct MaterialParams
{
    uint   BaseColor;        uint BaseColorSampler;   // unused handle slots (no texture sampling)
    uint   ORM;              uint ORMSampler;
    uint   Normal;           uint NormalSampler;
    uint   Pad0;             uint Pad1;
    float4 BaseColorFactor;  // the flat albedo
    float4 EmissiveFactor;   // rgb reserved; a = emissive strength
    float  MetallicFactor;   float RoughnessFactor;
    float  OcclusionStrength; float Pad2;
};

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

[shader("fragment")]
GBufferOutput fsMain(SurfaceFragmentInput input)
{
    MaterialParams p = LoadMaterialParams(input.v_MaterialIndex);

    GBufferOutput output;
    output.Albedo = p.BaseColorFactor;
    output.Normal = float4(normalize(input.v_WorldNormal), 0.0);
    output.ORM = float4(1.0, p.RoughnessFactor, p.MetallicFactor, p.EmissiveFactor.a);
    output.Velocity = ComputeMotionVector(input.v_CurClip, input.v_PrevClip);
    return output;
}
