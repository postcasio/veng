// Evaluates the sphere terrain tile mapping on the device, so a test can hold the device's f32
// arithmetic to the double-precision reference: per sample, the direction offset from the tile's
// centre (xyz) and the u face-coordinate fraction (w).
#include "Veng/sphere_terrain_mapping.slang"

struct ProbeSample
{
    uint Tile;
    float S;
    float T;
    uint Pad;
};

[[vk::binding(0, 3)]] StructuredBuffer<SphereTerrainTileConstants> g_Tiles;
[[vk::binding(1, 3)]] StructuredBuffer<ProbeSample> g_Samples;
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
    const ProbeSample probe = g_Samples[id.x];
    const SphereTerrainTileConstants tile = g_Tiles[probe.Tile];
    const float3 offset = SphereTerrainTileOffset(tile, float2(probe.S, probe.T));
    const float fraction = SphereTerrainTileFraction(probe.S, tile.Warp.x, tile.Warp.y);
    g_Results[id.x] = float4(offset, fraction);
}
