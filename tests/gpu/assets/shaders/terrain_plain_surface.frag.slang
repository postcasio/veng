#include "Veng/surface.slang"

// An ordinary Surface fragment (gpu fixture): the authored base colour, flat. Drawn on meshes, and
// handed to a sphere terrain to show a terrain refuses a material not declaring "sphereTerrain".
struct MaterialParams
{
    float4 BaseColor;
};

[shader("fragment")]
GBufferOutput fsMain(SurfaceFragmentInput input)
{
    const MaterialParams p = g_MaterialParams.Load<MaterialParams>(input.v_MaterialOffset);
    GBufferOutput output;
    output.Albedo = float4(p.BaseColor.rgb, 1.0);
    output.Normal = float4(normalize(input.v_WorldNormal), 0.0);
    output.ORM = float4(1.0, 0.8, 0.0, 0.0);
    output.Velocity = ComputeMotionVector(input.v_CurClip, input.v_PrevClip);
    output.Emissive = float3(0.0);
    return output;
}
