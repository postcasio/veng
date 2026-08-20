#include "Veng/translucent.slang"
#include "Veng/octahedral.slang"

// A CaptureSurface monitor material (gpu fixture) whose capture slots carry descriptive, non-default
// names (CaptureMap / CaptureSampler rather than Texture / Sampler). It exercises CaptureSurface's
// configurable TextureSlot / SamplerSlot: the component binds its output onto these named slots, and
// the shader samples the octahedral map exactly as capture_probe does — so a green center proves the
// non-default slot names bound end to end.
struct MaterialParams
{
    uint CaptureMap;
    uint CaptureSampler;
};

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

[shader("fragment")]
float4 fsMain(SurfaceFragmentInput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(input.v_MaterialIndex);
    const float2 uv = OctahedralUV(float3(0.0, -1.0, 0.0));
    const float3 color = g_Textures[NonUniformResourceIndex(p.CaptureMap)]
                             .Sample(g_Samplers[NonUniformResourceIndex(p.CaptureSampler)], uv)
                             .rgb;
    return float4(color, 1.0);
}
