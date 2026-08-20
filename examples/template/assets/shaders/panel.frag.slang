#include "Veng/translucent.slang"

// The glowing diegetic panel material: a translucent, self-radiant world surface that shows a live
// Gui::Document. GuiSurface binds the document's HDR render target onto the Texture/Sampler handle
// fields each frame; the fragment returns the sampled texel as the surface's radiance so bright
// document pixels bloom through the scene's own bloom. The Gui target is premultiplied alpha but the
// Translucent domain blends STRAIGHT alpha, so the color is un-premultiplied (rgb / a): a transparent
// texel yields zero coverage and shows the scene behind, a bright texel glows.
//
// For a solid monitor that occludes what is behind it, an OpaqueEmissive GuiSurface binds the same
// document handle onto an opaque Surface-domain material's EmissiveTexture instead — the bezel is a
// lit g-buffer surface and the document drives the additive emissive term.
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
