#include "Veng/surface.slang"

// A GuiSurface OpaqueEmissive panel material (gpu fixture): a solid, occluding g-buffer surface
// whose EmissiveColor / EmissiveTexture / EmissiveSampler fields GuiSurface binds each frame. The
// fragment samples the bound document target and writes EmissiveColor.rgb * texel into the emissive
// g-buffer channel (G4); the deferred lighting pass adds it, so the panel glows over a dark, lit
// bezel. The fields are referenced so the cooker reflects them into the parameter block GuiSurface
// writes.
struct MaterialParams
{
    float4 EmissiveColor;
    uint EmissiveTexture;
    uint EmissiveSampler;
    uint Pad0;
    uint Pad1;
};

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
    float3 v_WorldNormal : NORMAL;
    nointerpolation uint v_MaterialIndex : TEXCOORD1;
    float4 v_CurClip : TEXCOORD2;
    float4 v_PrevClip : TEXCOORD3;
};

[shader("fragment")]
GBufferOutput fsMain(VSOutput input)
{
    MaterialParams p = LoadMaterialParams(input.v_MaterialIndex);

    // A dark bezel; the emissive channel carries the document glow, sampled from the bound target.
    float3 albedo = float3(0.02, 0.02, 0.02);
    float3 emissive = float3(0.0);
    if (p.EmissiveTexture != 0u)
    {
        const float3 texel = g_Textures[NonUniformResourceIndex(p.EmissiveTexture)].Sample(
                                 g_Samplers[NonUniformResourceIndex(p.EmissiveSampler)], input.v_UV).rgb;
        emissive = p.EmissiveColor.rgb * texel;
    }

    GBufferOutput output;
    output.Albedo = float4(albedo, 1.0);
    output.Normal = float4(normalize(input.v_WorldNormal), 0.0);
    output.ORM = float4(1.0, 1.0, 0.0, 0.0);
    output.Velocity = ComputeMotionVector(input.v_CurClip, input.v_PrevClip);
    output.Emissive = emissive;
    return output;
}
