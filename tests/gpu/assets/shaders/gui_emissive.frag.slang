#include "Veng/surface.slang"

// A GuiSurface OpaqueEmissive panel material (gpu fixture): a solid, occluding g-buffer surface
// whose EmissiveColor / EmissiveTexture / EmissiveSampler fields GuiSurface binds each frame. The
// forward emissive pass reads those fields through MaterialInstance::GetEmissive and composites
// EmissiveColor * document-texel additively pre-bloom, so the g-buffer stage here only writes a
// dark, lit bezel — the glow comes from the emissive term. The fields are referenced so the cooker
// reflects them into the parameter block GuiSurface writes.
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

    // A dark bezel; the emissive term carries the document glow. The handle fields are referenced
    // so the reflection keeps them in the block GuiSurface binds.
    float3 albedo = float3(0.02, 0.02, 0.02);
    if (p.EmissiveTexture != 0u)
    {
        albedo += 0.0 * g_Textures[NonUniformResourceIndex(p.EmissiveTexture)].Sample(
                            g_Samplers[NonUniformResourceIndex(p.EmissiveSampler)], input.v_UV).rgb;
    }

    GBufferOutput output;
    output.Albedo = float4(albedo, 1.0);
    output.Normal = float4(normalize(input.v_WorldNormal), 0.0);
    output.ORM = float4(1.0, 1.0, 0.0, p.EmissiveColor.a);
    output.Velocity = ComputeMotionVector(input.v_CurClip, input.v_PrevClip);
    output.Emissive = float3(0.0);
    return output;
}
