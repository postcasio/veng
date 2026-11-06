#include "Veng/sphere_terrain_translucent.slang"

// Translucent level-layer fragment fixture: reads the tile record through the terrain set at 6 and
// returns a straight colour and coverage. Used to assert a Translucent "sphereTerrain" material cooks
// against the core translucent terrain stage, and that set 6 is refused without the key.
[shader("fragment")]
float4 fsMain(SphereTerrainFragmentInput input) : SV_Target0
{
    const float texel = TerrainTexelSize(input);
    return float4(texel, TerrainMorph(input), float(TerrainSurface(input)), 0.5);
}
