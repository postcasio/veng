#include "Veng/surface.slang"
#include "Veng/light_visibility.slang"

// A shadow-reading Surface fragment (gpu fixture): writes no albedo and, as its emissive, the
// share of the light tagged Select.x reaching its pixel through the shadow maps — the world
// position the deferred pass reconstructs and the normal it writes, through the 3×3 kernel. With
// Select.y > 0.5 it writes a marker emissive of 2 instead, which no visibility reaches, so a test
// can tell the surfaces it reads apart from the casters standing over them. It leaves light channel
// 0 for channel 1, so a light on channel 0 adds nothing of its own and the lit radiance the
// deferred pass writes is the emissive alone.
struct MaterialParams
{
    float4 Select;
};

[shader("fragment")]
GBufferOutput fsMain(SurfaceFragmentInput input)
{
    const MaterialParams p = g_MaterialParams.Load<MaterialParams>(input.v_MaterialOffset);
    const ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);
    const float3 normal = normalize(input.v_WorldNormal);
    const float visibility =
        LightVisibility(vc, FindLightByTag(vc, uint(p.Select.x)),
                        SurfaceWorldPosition(input.sv_position), normal, LightVisibilityPcf);

    GBufferOutput output;
    output.Albedo = float4(0.0, 0.0, 0.0, 1.0);
    output.Normal = float4(normal, 0.0);
    output.ORM = float4(1.0, 1.0, 0.0, SurfaceLightChannels(0x2u));
    output.Velocity = ComputeMotionVector(input.v_CurClip, input.v_PrevClip);
    output.Emissive = p.Select.y > 0.5 ? float3(2.0) : float3(visibility);
    return output;
}
