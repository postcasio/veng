// A sky field material: three field targets, each a different known, smooth function of the world
// view direction, so a later layer's reconstruction of each is checkable against the function. The
// third is quartic, whose solid-angle mean (1/5 per axis) a texel-count mean misses by about 0.02.
#include "Veng/sky.slang"

struct MaterialParams
{
    float4 Scale; // an authored param so the block is non-empty
};

[[vk::push_constant]] ConstantBuffer<SkyPushConstants> g_SkyPC;

struct FieldOutput
{
    [[vk::location(0)]] float4 Target0 : SV_Target0;
    [[vk::location(1)]] float4 Target1 : SV_Target1;
    [[vk::location(2)]] float4 Target2 : SV_Target2;
};

[shader("fragment")]
FieldOutput fsMain(SkyFragmentInput input)
{
    const MaterialParams p = g_MaterialParams.Load<MaterialParams>(g_SkyPC.MaterialOffset);
    const float3 d = normalize(SkyViewDirection(g_SkyPC, input.v_UV));
    FieldOutput o;
    o.Target0 = float4(0.5 + 0.5 * d, 1.0) * p.Scale;
    o.Target1 = float4(d.x * d.y, d.y * d.z, d.z * d.x, 0.5) * p.Scale;
    const float3 d2 = d * d;
    o.Target2 = float4(d2 * d2, 1.0) * p.Scale;
    return o;
}
