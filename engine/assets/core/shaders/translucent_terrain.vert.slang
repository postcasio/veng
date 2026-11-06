// Translucent vertex stage for a sphere terrain's level layer: the terrain set at 6, past the forward
// lighting sets 4 and 5 (SphereTerrainVertexStage).
#define VE_SPHERE_TERRAIN_SET 6
#include "Veng/sphere_terrain.slang"

[shader("vertex")]
SphereTerrainFragmentInput vsMain(uint vertexIndex : SV_VertexID, uint listIndex : SV_VulkanInstanceID)
{
    return SphereTerrainVertexStage(vertexIndex, listIndex);
}
