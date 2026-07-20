// Depth-of-field tile reduction + dilation: one invocation owns one tile of the half-resolution
// circle-of-confusion buffer and reduces the tile plus its eight neighbours into a single record
// — the minimum view-space depth and the largest near- and far-field radii anywhere in that
// three-by-three block. Reducing over the dilated neighbourhood in one pass is what makes a
// gather see the near-field spill its neighbours advertise; the record then bounds each gather's
// kernel radius, so an in-focus tile costs one tap.
//
// The reduction mirrors AccumulateDofTile / MergeDofTiles (Veng/Renderer/DofTile.h): a
// non-positive depth carries no defocus and contributes nothing, the near field is the negative
// side of the signed radius and the far field the positive side.
//
// Set 0 is reserved for the bindless registry; this stage binds the shared sampled-source +
// sampler + storage-destination set on set 1.
[[vk::binding(0, 1)]] Texture2D<float2> g_Coc;
[[vk::binding(1, 1)]] SamplerState g_Sampler;
[[vk::binding(2, 1)]] [format("rgba16f")] RWTexture2D<float4> g_Dest;

static const int TileSize = 8;

struct PushConstants
{
    uint2 DestExtent;    // tile grid extent
    uint2 SourceExtent;  // half-resolution valid extent in texels
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_PC.DestExtent.x || id.y >= g_PC.DestExtent.y)
        return;

    int2 base = int2(id.xy) * TileSize;
    int2 last = int2(g_PC.SourceExtent) - 1;

    float minDepth = 1e30;
    float maxNear = 0.0;
    float maxFar = 0.0;

    // The tile itself plus one ring of neighbouring tiles — the dilation, folded into the
    // reduction rather than run as a second pass over a reduced grid.
    for (int y = -TileSize; y < 2 * TileSize; ++y)
    {
        for (int x = -TileSize; x < 2 * TileSize; ++x)
        {
            int2 p = clamp(base + int2(x, y), int2(0, 0), last);
            float2 sample = g_Coc.Load(int3(p, 0));
            if (sample.y <= 0.0)
                continue;
            minDepth = min(minDepth, sample.y);
            maxNear = max(maxNear, max(-sample.x, 0.0));
            maxFar = max(maxFar, max(sample.x, 0.0));
        }
    }

    // A tile whose every texel was behind the camera has no depth; report zero rather than the
    // sentinel, which would store as infinity in the half-float target.
    float depth = minDepth >= 1e30 ? 0.0 : minDepth;
    g_Dest[int2(id.xy)] = float4(depth, maxNear, maxFar, 0.0);
}
