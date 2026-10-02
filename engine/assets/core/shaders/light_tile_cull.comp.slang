// The light-tile cull: one workgroup per LightTileSize-square screen tile writes that tile's light
// mask (Veng/light_tiles.slang) — the view's lights that can reach any surface the tile shows — so
// the deferred lighting pass visits only those at each of the tile's pixels.
//
// A workgroup reduces the tile's depth to its nearest and farthest geometry (reverse-Z: the largest
// and smallest values above the cleared 0), bounds the slice of the tile's frustum between those two
// depths by the view-space box around its eight corners, and tests each light's influence sphere
// against that box. The test is conservative, so the image is unchanged and only the work moves:
// - Every pixel the lighting pass shades reconstructs its position from the same jittered
//   projection, at a pixel centre strictly inside the tile's edges and at a depth inside the
//   reduced range, so it lies inside the slice, and the box holds the slice.
// - A light's sphere holds every point it lights. A point or spot light lights nothing at or past
//   its range from its position; a spot is bounded by the sphere alone. An area light's cutoff is
//   measured from its emitter's surface, which lies within the emitter's reach (a Sphere's radius, a
//   Rect's or Polygon's farthest vertex) of its position, so the sphere is the range grown by it.
// - The sphere is grown further by a relative epsilon covering the float error of the lighting
//   pass's world-space reconstruction, which this pass computes camera-relative.
// A directional light has no range and is in every tile's mask. A tile showing no geometry gets no
// positioned light: the lighting pass shades none of its pixels.
//
// The cull reads the depth target through the bindless set-0 textures, and the view's lights and
// mask placement through the view block; it writes the mask buffer through its own set 3.
#include "view_constants.slang"
#include "Veng/light_buffer.slang"
#include "Veng/light_tiles.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];

// The renderer's ring of per-tile masks; LightTileWord places this frame's.
[[vk::binding(0, 3)]] RWByteAddressBuffer g_TileMasks;

struct PushConstants
{
    uint ViewConstantsIndex; // current view's view-constants region (lights, extents, mask placement)
    uint DepthTexture;       // the g-buffer depth's bindless index
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

// The relative growth applied to every light's sphere: far above the float error of reconstructing
// a world position through a 4x4 inverse projection, and far below any distance worth culling at.
static const float BoundEpsilon = 1e-5;

groupshared uint s_NearDepthBits;
groupshared uint s_FarDepthBits;
groupshared float3 s_BoxMin;
groupshared float3 s_BoxMax;
groupshared uint s_Mask;

// One corner of the tile's frustum slice in view space, reconstructed camera-relative through the
// translation-free inverse so its precision does not depend on where the camera stands.
float3 SliceCornerView(ViewConstants vc, float2 ndc, float depth)
{
    float4 h = mul(vc.InvViewRotProj, float4(ndc, depth, 1.0));
    return mul(vc.View, float4(h.xyz / h.w, 0.0)).xyz;
}

// The world-space sphere outside which a positioned light adds exactly nothing, or false for a
// directional light, which has none.
bool LightInfluenceSphere(ViewConstants vc, GpuLight light, out float3 center, out float radius)
{
    center = light.PositionRange.xyz;
    radius = 0.0;
    float type = light.DirectionType.w;
    if (type == LightTypeDirectional)
    {
        return false;
    }

    float reach = 0.0;
    if (type == LightTypeSphere)
    {
        reach = max(light.Area.x, 0.0);
    }
    else if (type > LightTypeSpot + 0.5)
    {
        uint base = uint(light.Area.y);
        uint count = uint(light.Area.z);
        if (count >= 3u)
        {
            for (uint i = 0u; i < count; ++i)
            {
                reach = max(reach, length(LoadAreaVertex(vc, base + i) - center));
            }
        }
    }
    radius = max(light.PositionRange.w, 1e-4) + reach;
    return true;
}

[shader("compute")]
[numthreads(LightTileSize, LightTileSize, 1)]
void csMain(uint3 tile : SV_GroupID, uint3 local : SV_GroupThreadID, uint groupIndex : SV_GroupIndex)
{
    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);
    uint2 validExtent = uint2(vc.ExtentParams.xy);

    if (groupIndex == 0u)
    {
        s_NearDepthBits = 0u;
        s_FarDepthBits = 0xFFFFFFFFu;
        s_Mask = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    // Depth is non-negative, so its bit pattern orders as the value does and an integer min/max
    // reduces it. A cleared texel (0, no geometry) is left out, as the lighting pass leaves it unlit.
    uint2 pixel = tile.xy * LightTileSize + local.xy;
    if (all(pixel < validExtent))
    {
        float depth = g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)].Load(int3(pixel, 0)).r;
        if (depth > 0.0)
        {
            InterlockedMax(s_NearDepthBits, asuint(depth));
            InterlockedMin(s_FarDepthBits, asuint(depth));
        }
    }
    GroupMemoryBarrierWithGroupSync();

    bool hasGeometry = s_NearDepthBits != 0u;
    if (groupIndex == 0u && hasGeometry)
    {
        // The tile's pixel edges in NDC, by the same uv * 2 - 1 the lighting pass reconstructs with.
        float2 extent = float2(validExtent);
        float2 lo = float2(tile.xy * LightTileSize) / extent * 2.0 - 1.0;
        float2 hi = float2(min((tile.xy + 1u) * LightTileSize, validExtent)) / extent * 2.0 - 1.0;
        float nearDepth = asfloat(s_NearDepthBits);
        float farDepth = asfloat(s_FarDepthBits);

        float3 boxMin = float3(3.4e38);
        float3 boxMax = float3(-3.4e38);
        for (uint corner = 0u; corner < 8u; ++corner)
        {
            float2 ndc = float2((corner & 1u) != 0u ? hi.x : lo.x, (corner & 2u) != 0u ? hi.y : lo.y);
            float3 p = SliceCornerView(vc, ndc, (corner & 4u) != 0u ? nearDepth : farDepth);
            boxMin = min(boxMin, p);
            boxMax = max(boxMax, p);
        }
        s_BoxMin = boxMin;
        s_BoxMax = boxMax;
    }
    GroupMemoryBarrierWithGroupSync();

    // One thread per light: the view holds at most MaxLights, well under a workgroup.
    if (groupIndex < vc.LightState.y && groupIndex < 32u)
    {
        GpuLight light = LoadLight(vc, groupIndex);
        float3 center;
        float radius;
        bool reaches;
        if (!LightInfluenceSphere(vc, light, center, radius))
        {
            reaches = true;
        }
        else if (!hasGeometry)
        {
            reaches = false;
        }
        else
        {
            float3 rel = center - vc.CameraPosition.xyz;
            float3 c = mul(vc.View, float4(rel, 0.0)).xyz;
            float bound = radius + BoundEpsilon * (radius + length(rel) + length(vc.CameraPosition.xyz));
            float3 d = c - clamp(c, s_BoxMin, s_BoxMax);
            reaches = dot(d, d) <= bound * bound;
        }
        if (reaches)
        {
            InterlockedOr(s_Mask, 1u << groupIndex);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (groupIndex == 0u)
    {
        g_TileMasks.Store(LightTileWord(vc, tile.xy) * 4u, s_Mask);
    }
}
