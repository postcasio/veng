// Evaluates sphere terrain's vertex placement on the device, so a test can hold the device's f32
// arithmetic to the double-precision reference: per sample, the eye-relative position (xyz).
#include "Veng/sphere_terrain_draw.slang"

struct PlacementSample
{
    uint Record;
    float S;
    float T;
    float Height;
};

[[vk::binding(0, 3)]] StructuredBuffer<SphereTerrainRecord> g_Records;
[[vk::binding(1, 3)]] StructuredBuffer<PlacementSample> g_Samples;
[[vk::binding(2, 3)]] RWStructuredBuffer<float4> g_Results;

struct ProbePush
{
    uint Count;
};
[[vk::push_constant]] ProbePush u_Push;

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= u_Push.Count)
    {
        return;
    }
    const PlacementSample probe = g_Samples[id.x];
    const SphereTerrainPlacement placed =
        SphereTerrainPlace(g_Records[probe.Record], float2(probe.S, probe.T), probe.Height);
    g_Results[id.x] = float4(placed.Position, 0.0);
}
