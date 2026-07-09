#include "Veng/translucent.slang"
#include "Veng/octahedral.slang"

// A diegetic reflective panel material: samples the CaptureSurface's octahedral scene capture in the
// surface's world-normal direction, so the panel shows the scene captured from its entity — a mirror
// facing back into the world. CaptureSurface binds the capture's octahedral output onto the
// Texture/Sampler handle fields each frame (a runtime bindless slot, not a cooked id); the fragment
// returns the sampled pre-tonemap HDR color as the surface's radiance, so a bright captured pixel
// blooms through the scene's own bloom.
struct MaterialParams
{
    uint Texture;
    uint Sampler;
    uint Pad0;
    uint Pad1;
};

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

[shader("fragment")]
float4 fsMain(SurfaceFragmentInput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(input.v_MaterialIndex);
    const float2 uv = OctahedralUV(normalize(input.v_WorldNormal));
    const float3 color = g_Textures[NonUniformResourceIndex(p.Texture)]
                             .Sample(g_Samplers[NonUniformResourceIndex(p.Sampler)], uv)
                             .rgb;
    return float4(color, 1.0);
}
