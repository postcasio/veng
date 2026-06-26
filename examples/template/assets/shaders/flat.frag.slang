#include "material_data.slang"

// The template's trivial surface fragment stage: it writes a flat, untextured
// metallic-roughness surface into the g-buffer straight from the authored
// MaterialParams scalars — no texture sampling, no normal map. BaseColorFactor is the
// albedo, the metallic/roughness factors are the packed ORM channels, and occlusion is
// fully open. It is the smallest fragment that drives the deferred lighting pass to a
// lit, solid-colour cube.
struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
    float3 v_WorldNormal : NORMAL;
    float4 v_WorldTangent : TANGENT;  // xyz world tangent; w handedness sign for the bitangent
    nointerpolation uint v_MaterialIndex : TEXCOORD1;  // per-draw material selector from the vertex stage
    float4 v_CurClip : TEXCOORD2;   // unjittered current clip position (g-buffer motion vector)
    float4 v_PrevClip : TEXCOORD3;  // unjittered previous clip position (g-buffer motion vector)
};

[shader("fragment")]
GBufferOutput fsMain(VSOutput input)
{
    MaterialParams p = LoadMaterialParams(input.v_MaterialIndex);

    GBufferOutput output;
    output.Albedo = p.BaseColorFactor;
    output.Normal = float4(normalize(input.v_WorldNormal), 0.0);
    output.ORM = float4(1.0, p.RoughnessFactor, p.MetallicFactor, p.EmissiveFactor.a);
    output.Velocity = ComputeMotionVector(input.v_CurClip, input.v_PrevClip);
    return output;
}
