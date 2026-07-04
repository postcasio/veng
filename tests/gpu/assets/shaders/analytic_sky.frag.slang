// An analytic Sky-domain material: it writes a continuous, known function of the world-space
// view direction — 0.5 + 0.5 * dir, encoding the direction into rgb. Used to exercise the sky
// cubemap bake: because the radiance is a pure function of direction, adjacent cube faces must
// agree exactly along every shared edge (the seam-continuity oracle), and each baked texel's
// value is predictable from the direction its face basis reconstructs.
#include "Veng/sky.slang"

struct MaterialParams
{
    float4 Scale; // rgb scale on the encoded direction; a authored param so the block is non-empty
};

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

[[vk::push_constant]] ConstantBuffer<SkyPushConstants> g_SkyPC;

[shader("fragment")]
float4 fsMain(SkyFragmentInput input) : SV_Target0
{
    if (!SkyIsBackground(g_SkyPC, input.v_UV))
    {
        discard;
    }

    MaterialParams p = LoadMaterialParams(g_SkyPC.MaterialIndex);
    float3 dir = SkyViewDirection(g_SkyPC, input.v_UV);
    return float4((0.5 + 0.5 * dir) * p.Scale.rgb, 1.0);
}
