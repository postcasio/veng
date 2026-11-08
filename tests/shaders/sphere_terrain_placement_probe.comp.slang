// Evaluates sphere terrain's vertex placement on the device, so a test can hold the device's f32
// arithmetic to the double-precision reference: per sample, the eye-relative position (xyz).
#include "Veng/sphere_terrain_draw.slang"
#include "Veng/binding.slang"

struct PlacementSample
{
    uint Record;
    float S;
    float T;
    float Height;
};

struct SphereTerrainPlacementProbeSet3
{
    StructuredBuffer<SphereTerrainRecord> Records;
    StructuredBuffer<PlacementSample> Samples;
    RWStructuredBuffer<float4> Results;
};
VE_DESCRIPTOR_SET(3, SphereTerrainPlacementProbeSet3, g_Set3);

struct ProbePush
{
    uint Count;
};
VE_PUSH_CONSTANTS(ProbePush, u_Push);

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= u_Push.Count)
    {
        return;
    }
    const PlacementSample probe = g_Set3.Samples[id.x];
    const SphereTerrainPlacement placed =
        SphereTerrainPlace(g_Set3.Records[probe.Record], float2(probe.S, probe.T), probe.Height);
    g_Set3.Results[id.x] = float4(placed.Position, 0.0);
}
