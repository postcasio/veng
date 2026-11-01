// A sky composite that reads a field bake's field: one target, filtered bicubically (mode 0), its
// solid-angle mean (mode 1) or one mip (mode 2), scaled — so a test checks each read by what it
// writes into the cube.
#include "Veng/sky_bake.slang"

struct MaterialParams
{
    float4 Mode;  // x the target, y the read (0 bicubic, 1 mean, 2 mip), z the mip
    float4 Scale; // multiplies what is read
};

[[vk::push_constant]] ConstantBuffer<SkyPushConstants> g_SkyPC;

[shader("fragment")]
float4 fsMain(SkyFragmentInput input) : SV_Target0
{
    const MaterialParams p = g_MaterialParams.Load<MaterialParams>(g_SkyPC.MaterialOffset);
    const float3 dir = SkyViewDirection(g_SkyPC, input.v_UV);
    const uint target = uint(p.Mode.x);
    float4 value;
    if (p.Mode.y > 1.5)
    {
        value = SkyBakeFieldLod(g_SkyPC, target, dir, p.Mode.z);
    }
    else if (p.Mode.y > 0.5)
    {
        value = SkyBakeFieldMean(g_SkyPC, target);
    }
    else
    {
        value = SkyBakeField(g_SkyPC, target, dir);
    }
    return value * p.Scale;
}
