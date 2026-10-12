#include "Veng/instanced_mesh.slang"

// A Surface fragment an instanced mesh set may draw (gpu fixture): a flat diffuse surface whose
// albedo is the authored base colour, tinted by the instance's datum where an instanced set drew
// it (an ordinary mesh reads the zero datum, so it is left untinted).
struct MaterialParams
{
    float4 BaseColor;
    float  Roughness;
};

[shader("fragment")]
GBufferOutput fsMain(SurfaceFragmentInput input)
{
    const MaterialParams p = g_MaterialParams.Load<MaterialParams>(input.v_MaterialOffset);
    const float3 tint = IsInstanceDraw(input) ? InstanceDatum(input).rgb : float3(1.0);

    GBufferOutput output;
    output.Albedo = float4(p.BaseColor.rgb * tint, 1.0);
    output.Normal = float4(normalize(input.v_WorldNormal), 0.0);
    output.ORM = float4(1.0, p.Roughness, 0.0, 0.0);
    output.Velocity = ComputeMotionVector(input.v_CurClip, input.v_PrevClip);
    output.Emissive = float3(0.0);
    return output;
}
