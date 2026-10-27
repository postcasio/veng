// The sphere terrain test generator: height as a few sinusoids of the texel's direction, evaluated
// through the engine's precise tile mapping. Its parameters ride a small storage buffer so the
// tile's constants can fill the push block. A tile at or past FlatFromLevel holds zero, so a test
// can make a level carry no detail.
#include "Veng/sphere_terrain_mapping.slang"

[[vk::binding(0, 3)]] [format("r32f")] RWTexture2D<float> g_Height;

struct GeneratorParams
{
    // The sinusoids' amplitude in metres.
    float Amplitude;
    // Their angular frequency, in radians per unit of direction.
    float Frequency;
    // The level from which the tile holds zero.
    uint FlatFromLevel;
    uint Pad;
};
[[vk::binding(1, 3)]] StructuredBuffer<GeneratorParams> g_Params;

[[vk::push_constant]] SphereTerrainTileConstants u_Tile;

// Three sinusoids of the direction; its gradient is bounded by Amplitude * Frequency * 3 per unit
// of direction, since each wave vector is a unit vector.
float TestHeight(float3 d, GeneratorParams params)
{
    const float f = params.Frequency;
    return params.Amplitude * (sin(f * d.x + 0.3) + sin(f * d.y + 1.1) + sin(f * dot(d, float3(0.6, 0.0, 0.8)) + 2.0));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    const uint texels = SphereTerrainTileTexels(u_Tile);
    if (any(id.xy >= uint2(texels, texels)))
    {
        return;
    }
    const GeneratorParams params = g_Params[0];
    float h = 0.0;
    if (SphereTerrainTileLevel(u_Tile) < params.FlatFromLevel)
    {
        h = TestHeight(SphereTerrainTexelDirection(u_Tile, id.xy, 1u), params);
    }
    g_Height[id.xy] = h;
}
