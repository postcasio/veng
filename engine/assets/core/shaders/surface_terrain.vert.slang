#include "Veng/sphere_terrain.slang"

// Deferred surface vertex stage for sphere terrain: the terrain set at 4 (SphereTerrainVertexStage).
[shader("vertex")]
SphereTerrainFragmentInput vsMain(uint vertexIndex : SV_VertexID, uint listIndex : SV_VulkanInstanceID)
{
    return SphereTerrainVertexStage(vertexIndex, listIndex);
}
