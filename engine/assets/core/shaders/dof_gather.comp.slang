// Depth-of-field ring gather: scatter-as-gather over concentric rings scaled by the circle of
// confusion, so the kernel footprint is a disc and a defocused point resolves as a round bokeh.
// One dispatch per layer — the near layer and the far layer differ only in whether a sample's
// radius is clamped against the destination's own.
//
// A sample contributes where its own circle of confusion reaches the destination texel,
// saturate(sampleCoc - distance + 1), which is the scatter test read backwards. The far layer
// additionally clamps each sample's radius to the destination's, so background blur can never
// bleed over in-focus foreground; the near layer is deliberately unclamped, which is exactly how
// a defocused foreground spills over sharp geometry behind it.
//
// The ring count is a per-frame push value so quality tunes with no recompile. It is ceilinged
// here by a compile-time MaxRings as a backstop: the CPU clamps it where it is pushed into the
// view state, and no missed clamp may ever produce an unbounded GPU loop.
//
// Set 0 is reserved for the bindless registry; this stage binds its own set 1.
[[vk::binding(0, 1)]] Texture2D<float4> g_Layer;
[[vk::binding(1, 1)]] Texture2D<float4> g_Tiles;
[[vk::binding(2, 1)]] SamplerState g_Sampler;
[[vk::binding(3, 1)]] [format("rgba16f")] RWTexture2D<float4> g_Dest;

static const uint MaxRings = 8;
static const float Tau = 6.28318530718;

struct PushConstants
{
    uint2 DestExtent;  // half-resolution valid extent in texels
    uint RingCount;    // gather rings; ceilinged by MaxRings below
    uint NearField;    // 1 = near layer (unclamped spill), 0 = far layer
    float MaxCoc;      // radius ceiling in half-resolution pixels
    float Pad0;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_PC.DestExtent.x || id.y >= g_PC.DestExtent.y)
        return;

    float2 texel = 1.0 / float2(g_PC.DestExtent);
    float2 uv = (float2(id.xy) + 0.5) * texel;

    float4 center = g_Layer.SampleLevel(g_Sampler, uv, 0.0);
    float4 tile = g_Tiles.SampleLevel(g_Sampler, uv, 0.0);

    float radius = min(g_PC.NearField != 0 ? tile.y : tile.z, g_PC.MaxCoc);
    if (radius < 0.5)
    {
        g_Dest[int2(id.xy)] = float4(center.rgb, saturate(center.a));
        return;
    }

    // The center always contributes with unit weight, so the accumulator can never be empty.
    float3 sum = center.rgb;
    float weightSum = 1.0;
    float coverageSum = saturate(center.a);

    uint rings = min(g_PC.RingCount, MaxRings);
    for (uint r = 1; r <= rings; ++r)
    {
        float ringRadius = radius * float(r) / float(rings);
        uint count = 8u * r;
        for (uint s = 0; s < count; ++s)
        {
            // Each ring is rotated by half a step relative to the last, so the samples of
            // successive rings interleave instead of lining up on spokes.
            float angle = (float(s) + 0.5 * float(r)) * (Tau / float(count));
            float2 offset = float2(cos(angle), sin(angle)) * ringRadius;
            float4 t = g_Layer.SampleLevel(g_Sampler, uv + offset * texel, 0.0);

            float sampleCoc = t.a;
            if (g_PC.NearField == 0)
                sampleCoc = min(sampleCoc, center.a);

            float w = saturate(sampleCoc - length(offset) + 1.0);
            sum += t.rgb * w;
            weightSum += w;
            coverageSum += w * saturate(sampleCoc);
        }
    }

    g_Dest[int2(id.xy)] = float4(sum / weightSum, saturate(coverageSum / weightSum));
}
