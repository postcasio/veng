#include "Veng/translucent.slang"

// A GuiSurface translucent panel material (gpu fixture): samples a runtime-bound document texture
// through the Texture/Sampler handle fields GuiSurface binds each frame and returns it as the
// surface's HDR radiance. The Gui render target holds premultiplied alpha, but the Translucent
// domain blends STRAIGHT (non-premultiplied) alpha, so the sampled color is un-premultiplied
// (rgb / a) to recover the straight-alpha color the blend expects — a transparent texel returns
// zero coverage and lets the scene behind show through, a bright texel blooms.
struct MaterialParams
{
    uint Texture;
    uint Sampler;
};

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

[shader("fragment")]
float4 fsMain(SurfaceFragmentInput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(input.v_MaterialIndex);
    float4 texel = g_Textures[NonUniformResourceIndex(p.Texture)].Sample(
        g_Samplers[NonUniformResourceIndex(p.Sampler)], input.v_UV);
    float3 straight = texel.a > 1e-4 ? texel.rgb / texel.a : float3(0.0);
    return float4(straight, texel.a);
}
