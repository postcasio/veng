#include "Veng/sphere_terrain.slang"

// Deferred surface vertex stage for sphere terrain. It reads no vertex attributes: the index names
// a vertex of the shared grid, the instance index names an entry of the view's tile list (the draw's
// firstInstance is the list's start), and the entry names the tile's record. The push block's
// InstanceDraw is the terrain's DrawData record (its material) and InstanceBuffer the surface drawn
// (0 the terrain, 1 + l its level layer l).
//
// The vertex is placed about the eye at true size, then scaled about the eye by the record's scale
// for the rasterized position alone: the motion vector and the true depth the fragment can write are
// the unscaled geometry's, so a scale about the eye, which moves nothing on screen, changes nothing
// but the rasterized depth.
[shader("vertex")]
SphereTerrainFragmentInput vsMain(uint vertexIndex : SV_VertexID, uint listIndex : SV_VulkanInstanceID)
{
    const uint surface = g_PC.InstanceBuffer;
    const SphereTerrainVertex v = SphereTerrainEvaluate(vertexIndex, listIndex, surface);
    const SphereTerrainRecord r = g_TerrainRecords[v.Record];
    const DrawData draw = LoadDrawData(g_PC.InstanceDraw);
    const ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);
    const float scale = r.ChordsB.w;

    const float3 camera = view.CameraPosition.xyz;
    const float3 rotated = mul((float3x3)view.View, v.Position * scale);

    SphereTerrainFragmentInput output;
    output.Surface.sv_position = mul(view.Proj, float4(rotated, 1.0));
    output.Surface.v_CurClip = mul(view.CurViewProj, float4(camera + v.Position, 1.0));
    output.Surface.v_PrevClip = mul(view.PrevViewProj, float4(camera + v.PrevPosition, 1.0));
    output.Surface.v_UV = SphereTerrainGridUV(v.St * float(SphereTerrainGridQuads));
    output.Surface.v_WorldNormal = v.Vertical;
    const float3 tangent = r.TangentU.xyz - v.Vertical * dot(v.Vertical, r.TangentU.xyz);
    output.Surface.v_WorldTangent = float4(normalize(tangent), 1.0);
    output.Surface.v_MaterialOffset = draw.MaterialOffset;
    output.Surface.v_CandidateId = g_PC.InstanceDraw;
    output.Surface.v_Instance = uint2(InstanceNone, 0u);
    const float3 offset = SphereTerrainTileOffset(r.Tile, v.St);
    output.Surface.v_ObjectPosition = r.ChordsB.z * offset + v.Height * (r.Tile.CenterHigh.xyz + offset);
    output.v_TerrainParentUV = SphereTerrainGridUV(v.ParentGrid);
    output.v_TerrainMorph = v.Morph;
    output.v_TerrainTile = uint2(v.Record, surface);
    return output;
}
