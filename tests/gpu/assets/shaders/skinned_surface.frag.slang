#include "Veng/surface.slang"

// Minimal Surface material fragment (gpu fixture) taking the full SurfaceFragmentInput the
// engine's Veng/surface.slang declares — the interpolant set both the static surface vertex
// stage (surface.vert / the surface_full.vert fixture) and the skinned one (surface_skinned.vert)
// write. It writes a solid authored base color into the g-buffer albedo, so a mesh drawn with it
// (static or skinned) reads clearly as non-background. Used by the skinned g-buffer regression.
struct MaterialParams
{
    float4 BaseColor;
};

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

[shader("fragment")]
GBufferOutput fsMain(SurfaceFragmentInput input)
{
    MaterialParams params = LoadMaterialParams(input.v_MaterialIndex);

    GBufferOutput output;
    output.Albedo = float4(params.BaseColor.rgb, 1.0);
    output.Normal = float4(normalize(input.v_WorldNormal), 0.0);
    output.ORM = float4(1.0, 1.0, 0.0, 0.0);
    output.Velocity = ComputeMotionVector(input.v_CurClip, input.v_PrevClip);
    output.Emissive = float3(0.0, 0.0, 0.0);
    return output;
}
