#include "Veng/translucent.slang"
#include "Veng/octahedral.slang"

// A capture-consuming material that reads the octahedral distance map (gpu fixture): it samples the
// map bound onto its Depth/DepthSampler slots in a fixed authored Direction and returns the recorded
// radial distance as radiance, so a rendered frame reads back the world-unit distance the capture
// measured in that direction. It stands in for a distance-marching consumer without the march's math:
// what it needs from the engine is the distance map, its point sampler, and the register that one
// OctahedralUV(direction) addresses the same surface the radiance map does.
struct MaterialParams
{
    float4 Direction; // World direction the distance map is sampled in.
    uint Depth;       // engine-bound: the octahedral distance map
    uint DepthSampler; // engine-bound: the point clamp sampler
};

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

[shader("fragment")]
float4 fsMain(SurfaceFragmentInput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(input.v_MaterialIndex);
    const float2 uv = OctahedralUV(normalize(p.Direction.xyz));
    const float distance = g_Textures[NonUniformResourceIndex(p.Depth)]
                               .Sample(g_Samplers[NonUniformResourceIndex(p.DepthSampler)], uv)
                               .r;
    return float4(distance, distance, distance, 1.0);
}
