#include "Veng/sphere_terrain_draw.slang"

// Depth-only vertex stage for sphere terrain's shadow casters: the same placement and morph as the
// g-buffer stage (SphereTerrainEvaluate), so a tile's shadow stands where its colour does. The push
// block carries the shadow view's raw view-projection, the camera position the records are relative
// to, and the surface drawn. A terrain casts only at scale one, so nothing here scales.
struct ShadowTerrainPushConstants
{
    float4x4 ViewProj; // scene -> light clip
    float4 Camera;     // xyz the camera position in scene space
    uint Surface;      // 0 the terrain, 1 + l its level layer l
    uint Pad0;
    uint Pad1;
    uint Pad2;
};

[[vk::push_constant]] ConstantBuffer<ShadowTerrainPushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
};

[shader("vertex")]
VSOutput vsMain(uint vertexIndex : SV_VertexID, uint listIndex : SV_VulkanInstanceID)
{
    const SphereTerrainVertex v = SphereTerrainEvaluate(vertexIndex, listIndex, g_PC.Surface);
    VSOutput output;
    output.sv_position = mul(g_PC.ViewProj, float4(g_PC.Camera.xyz + v.Position, 1.0));
    return output;
}
