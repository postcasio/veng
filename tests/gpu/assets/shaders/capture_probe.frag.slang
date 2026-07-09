#include "Veng/translucent.slang"
#include "Veng/octahedral.slang"

// A CaptureSurface monitor material (gpu fixture): samples the scene-capture octahedral map bound
// onto its Texture/Sampler slots by CaptureSurface each frame, in the fixed world direction the
// probe saw the scene content, and returns it as opaque HDR radiance — so the surface displays what
// the capture rendered from its entity's position. A fixed sample direction keeps the oracle simple:
// the surface shows the capture's -Y view, which the scene places out of the camera's direct view,
// so the sampled color can only reach the frame through the capture.
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
    const float2 uv = OctahedralUV(float3(0.0, -1.0, 0.0));
    const float3 color = g_Textures[NonUniformResourceIndex(p.Texture)]
                             .Sample(g_Samplers[NonUniformResourceIndex(p.Sampler)], uv)
                             .rgb;
    return float4(color, 1.0);
}
