// Evaluates the sphere terrain tile mapping on the device, so a test can hold the device's f32
// arithmetic to the double-precision reference: per sample, the direction offset from the tile's
// centre (xyz) and the u face-coordinate fraction (w).
#include "Veng/sphere_terrain_mapping.slang"
#include "Veng/binding.slang"

struct ProbeSample
{
    uint Tile;
    float S;
    float T;
    uint Pad;
};

struct SphereTerrainMappingProbeSet3
{
    StructuredBuffer<SphereTerrainTileConstants> Tiles;
    StructuredBuffer<ProbeSample> Samples;
    RWStructuredBuffer<float4> Results;
};
VE_DESCRIPTOR_SET(3, SphereTerrainMappingProbeSet3, g_Set3);

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
    const ProbeSample probe = g_Set3.Samples[id.x];
    const SphereTerrainTileConstants tile = g_Set3.Tiles[probe.Tile];
    const float3 offset = SphereTerrainTileOffset(tile, float2(probe.S, probe.T));
    const float fraction = SphereTerrainTileFraction(probe.S, tile.Warp.x, tile.Warp.y);
    g_Set3.Results[id.x] = float4(offset, fraction);
}
