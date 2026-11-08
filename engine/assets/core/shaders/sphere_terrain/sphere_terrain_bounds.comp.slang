// Measures one sphere terrain tile's height bound: the range of channel 0 over the tile's interior,
// and its MorphError — the largest difference, over the tile's 33 x 33 grid vertices, between the
// tile's height and its parent's grid interpolated there. Along a parent edge the parent's grid is
// linear; at a parent cell's centre the larger of its two diagonals' midpoints is taken, so the
// error holds whichever diagonal a draw triangulates the parent's cell with.
//
// One workgroup per tile. The tile's height layer binds as a storage image (it is in a storage
// layout while its tick runs); the parent's layer as a sampled image, resident and sampleable; the
// result as one entry of the frame's bounds buffer, bound alone so tiles never share a range.

#include "../Veng/binding.slang"

struct SphereTerrainBoundsSet3
{
    [format("r32f")] RWTexture2D<float> Height;
    Texture2D<float> ParentHeight;
    RWStructuredBuffer<float4> Bounds;
};
VE_DESCRIPTOR_SET(3, SphereTerrainBoundsSet3, g_Set3);

struct BoundsPush
{
    // The tile's quadrant within its parent, in parent half-tiles.
    uint2 Quadrant;
    // Nonzero when the tile has a parent to measure against.
    uint HasParent;
};
VE_PUSH_CONSTANTS(BoundsPush, u_Push);

static const uint Border = 2;
static const uint Cells = 128;
static const uint GridQuads = 32;
static const uint TexelsPerQuad = Cells / GridQuads;
static const uint GroupEdge = 16;
static const uint GroupSize = GroupEdge * GroupEdge;

groupshared float s_Min[GroupSize];
groupshared float s_Max[GroupSize];
groupshared float s_Error[GroupSize];

float ParentVertex(uint2 vertex)
{
    return g_Set3.ParentHeight.Load(int3(int2(Border + vertex * TexelsPerQuad), 0));
}

[shader("compute")]
[numthreads(16, 16, 1)]
void csMain(uint3 thread : SV_GroupThreadID)
{
    const uint index = thread.y * GroupEdge + thread.x;

    float low = 3.402823466e+38;
    float high = -3.402823466e+38;
    for (uint y = thread.y; y <= Cells; y += GroupEdge)
    {
        for (uint x = thread.x; x <= Cells; x += GroupEdge)
        {
            const float h = g_Set3.Height[int2(x + Border, y + Border)];
            low = min(low, h);
            high = max(high, h);
        }
    }

    float error = 0.0;
    if (u_Push.HasParent != 0u)
    {
        for (uint j = thread.y; j <= GridQuads; j += GroupEdge)
        {
            for (uint i = thread.x; i <= GridQuads; i += GroupEdge)
            {
                const float h = g_Set3.Height[int2(Border + i * TexelsPerQuad, Border + j * TexelsPerQuad)];
                // The vertex in parent half-vertex units: even is a parent vertex, odd a midpoint.
                const uint2 halfVertex = u_Push.Quadrant * GridQuads + uint2(i, j);
                const uint2 base = halfVertex >> 1;
                const uint2 odd = halfVertex & 1u;
                float e;
                if (odd.x == 0u && odd.y == 0u)
                {
                    e = abs(h - ParentVertex(base));
                }
                else if (odd.y == 0u)
                {
                    e = abs(h - 0.5 * (ParentVertex(base) + ParentVertex(base + uint2(1, 0))));
                }
                else if (odd.x == 0u)
                {
                    e = abs(h - 0.5 * (ParentVertex(base) + ParentVertex(base + uint2(0, 1))));
                }
                else
                {
                    const float a = 0.5 * (ParentVertex(base) + ParentVertex(base + uint2(1, 1)));
                    const float b = 0.5 * (ParentVertex(base + uint2(1, 0)) + ParentVertex(base + uint2(0, 1)));
                    e = max(abs(h - a), abs(h - b));
                }
                error = max(error, e);
            }
        }
    }

    s_Min[index] = low;
    s_Max[index] = high;
    s_Error[index] = error;
    GroupMemoryBarrierWithGroupSync();

    for (uint stride = GroupSize / 2; stride > 0; stride >>= 1)
    {
        if (index < stride)
        {
            s_Min[index] = min(s_Min[index], s_Min[index + stride]);
            s_Max[index] = max(s_Max[index], s_Max[index + stride]);
            s_Error[index] = max(s_Error[index], s_Error[index + stride]);
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (index == 0u)
    {
        g_Set3.Bounds[0] = float4(s_Min[0], s_Max[0], s_Error[0], 1.0);
    }
}
