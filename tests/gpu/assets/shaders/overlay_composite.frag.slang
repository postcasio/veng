// A GuiOverlay composite-material fixture — the glow-split contract on the PostProcess domain. It is
// run fullscreen by the GuiHdrOverlayScenePass over an overlay document the renderer first drew into
// an intermediate HDR target: it samples that document by integer pixel coordinate (the document is
// rasterized at the composite resolution, so the read is 1:1 with no sampler), un-premultiplies it,
// tints its color, and returns premultiplied-over color into the scene HDR plus a FLAT bloom
// amplitude into SV_Target1. The mask is what lets a dim element bloom by the amplitude it names
// rather than by how bright it is — the whole point of decoupling the halo from the drawn luminance.

#include "Veng/postprocess.slang"

struct MaterialParams
{
    float4 Tint;     // multiplies the document's straight (un-premultiplied) color
    float  Mask;     // flat bloom amplitude, coverage-weighted into SV_Target1
    uint   Document; // runtime-bound intermediate document handle (the composite writes it each frame)
};

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

// A PostProcess fragment declaring a bloom mask returns SV_Target0 (color) + SV_Target1 (amplitude).
struct CompositeOutput
{
    float4 Color     : SV_Target0;
    float  BloomMask : SV_Target1;
};

[shader("fragment")]
CompositeOutput fsMain(PostProcessFragmentInput input)
{
    MaterialParams p = LoadMaterialParams(g_PC.MaterialIndex);
    Texture2D document = g_Textures[NonUniformResourceIndex(p.Document)];

    const float4 texel = document.Load(int3(int2(input.sv_position.xy), 0)); // premultiplied
    const float coverage = texel.a;
    const float3 straight = coverage > 1e-4 ? texel.rgb / coverage : float3(0.0);
    const float3 shaped = straight * p.Tint.rgb;

    CompositeOutput output;
    output.Color = float4(shaped * coverage, coverage); // premultiplied-over
    output.BloomMask = p.Mask * coverage;               // flat amplitude, weighted by coverage
    return output;
}
