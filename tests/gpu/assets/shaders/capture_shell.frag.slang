#include "Veng/surface.slang"
#include "Veng/octahedral.slang"

// A CaptureSurface probe material that also occludes (gpu fixture): an opaque, depth-writing
// Surface-domain shell whose emissive channel carries the scene capture sampled in the authored
// Direction. Opaque and double-sided is what makes it an occlusion oracle as well as a colour one:
// the shell writes depth, so a capture that dropped it from colour while keeping it in depth would
// still hide the geometry beyond it, and the sampled direction would read black either way.
struct MaterialParams
{
    uint Texture;
    uint Sampler;
    uint Pad0;
    uint Pad1;
    float4 Direction; // World direction the capture's octahedral map is sampled in.
};

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

[shader("fragment")]
GBufferOutput fsMain(SurfaceFragmentInput input)
{
    MaterialParams p = LoadMaterialParams(input.v_MaterialIndex);

    const float2 uv = OctahedralUV(normalize(p.Direction.xyz));
    const float3 captured = g_Textures[NonUniformResourceIndex(p.Texture)]
                                .Sample(g_Samplers[NonUniformResourceIndex(p.Sampler)], uv)
                                .rgb;

    GBufferOutput output;
    output.Albedo = float4(0.0, 0.0, 0.0, 1.0);
    output.Normal = float4(normalize(input.v_WorldNormal), 0.0);
    output.ORM = float4(1.0, 1.0, 0.0, 0.0);
    output.Velocity = ComputeMotionVector(input.v_CurClip, input.v_PrevClip);
    output.Emissive = captured;
    return output;
}
