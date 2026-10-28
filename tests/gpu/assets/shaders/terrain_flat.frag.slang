#include "Veng/sphere_terrain.slang"

// A sphere-terrain fragment (gpu fixture): a flat diffuse surface in the authored base colour,
// brightened by the morph so the record and the interpolants are read, never darker than three
// quarters of it, under the tile overlay when the view asks for it.
struct MaterialParams
{
    float4 BaseColor;
};

GBufferOutput Shade(SphereTerrainFragmentInput input)
{
    const MaterialParams p = g_MaterialParams.Load<MaterialParams>(input.Surface.v_MaterialOffset);
    const float lift = 0.75 + 0.25 * TerrainMorph(input) * saturate(TerrainTexelSize(input));
    GBufferOutput output;
    output.Albedo = float4(TerrainTileOverlay(input, p.BaseColor.rgb * lift), 1.0);
    output.Normal = float4(normalize(input.Surface.v_WorldNormal), 0.0);
    output.ORM = float4(1.0, 0.8, 0.0, 0.0);
    output.Velocity = ComputeMotionVector(input.Surface.v_CurClip, input.Surface.v_PrevClip);
    output.Emissive = float3(0.0);
    return output;
}

[shader("fragment")]
GBufferOutput fsMain(SphereTerrainFragmentInput input)
{
    return Shade(input);
}
