#include "Veng/sphere_terrain.slang"

// A sphere-terrain fragment (gpu fixture) that writes the unscaled geometry's depth, so a terrain
// drawn at a scale about the eye stands at its true depth.
struct MaterialParams
{
    float4 BaseColor;
};

[shader("fragment")]
GBufferDepthOutput fsMain(SphereTerrainFragmentInput input)
{
    const MaterialParams p = g_MaterialParams.Load<MaterialParams>(input.Surface.v_MaterialOffset);
    GBufferOutput output;
    output.Albedo = float4(p.BaseColor.rgb, 1.0);
    output.Normal = float4(normalize(input.Surface.v_WorldNormal), 0.0);
    output.ORM = float4(1.0, 0.8, 0.0, 0.0);
    output.Velocity = ComputeMotionVector(input.Surface.v_CurClip, input.Surface.v_PrevClip);
    output.Emissive = float3(0.0);
    return WithDepth(output, TerrainTrueDeviceDepth(input));
}
