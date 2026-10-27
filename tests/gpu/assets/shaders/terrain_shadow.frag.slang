#include "Veng/sphere_terrain.slang"
#include "Veng/light_visibility.slang"

// A shadow-reading sphere-terrain fragment (gpu fixture): writes no albedo and, as its emissive,
// the share of the light tagged Select.x reaching its pixel through the shadow maps along the
// sphere's vertical. It leaves light channel 0 for channel 1, so a light on channel 0 adds nothing
// of its own and the lit target holds the visibility alone.
struct MaterialParams
{
    float4 Select;
};

[shader("fragment")]
GBufferOutput fsMain(SphereTerrainFragmentInput input)
{
    const MaterialParams p = g_MaterialParams.Load<MaterialParams>(input.Surface.v_MaterialOffset);
    const ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);
    const float3 normal = normalize(input.Surface.v_WorldNormal);
    const float visibility =
        LightVisibility(vc, FindLightByTag(vc, uint(p.Select.x)),
                        SurfaceWorldPosition(input.Surface.sv_position), normal,
                        LightVisibilityPcf);

    GBufferOutput output;
    output.Albedo = float4(0.0, 0.0, 0.0, 1.0);
    output.Normal = float4(normal, 0.0);
    output.ORM = float4(1.0, 1.0, 0.0, SurfaceLightChannels(0x2u));
    output.Velocity = ComputeMotionVector(input.Surface.v_CurClip, input.Surface.v_PrevClip);
    output.Emissive = float3(visibility);
    return output;
}
