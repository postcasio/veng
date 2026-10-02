// Refraction blur chain, coarse tail: the chain's smallest levels halved by one workgroup in one
// dispatch. Each of those levels is a handful of texels, so as separate passes they are almost
// entirely per-pass overhead; here the workgroup computes them level by level in shared memory and
// synchronizes between levels instead.
//
// Each level is exactly scene_color_downsample's: over the level's valid sub-rect (the render
// extent halved), four bilinear taps at the parent's texel corners around the destination texel's
// centre, each clamped inside the parent's valid region, averaged; outside the sub-rect the level
// holds the (0, 0, 0, 1) that pass's clear leaves. Level First is filtered from level First-1, which
// an earlier pass wrote, through the chain sampler; every deeper level from the shared copy of the
// level above it, with the same bilinear taps evaluated by hand. Shared memory holds each level at
// the precision its mip stores (RGB as halves).
#define TAIL_CAPACITY 3584
#define THREADS 256

[[vk::binding(0, 3)]] Texture2D<float4> g_Source;
[[vk::binding(1, 3)]] SamplerState g_Sampler;

// One storage binding per tail slot (bindings 2..9), the slots past LevelCount bound but unwritten.
[[vk::binding(2, 3)]] [format("rgba16f")] RWTexture2D<float4> g_Level0;
[[vk::binding(3, 3)]] [format("rgba16f")] RWTexture2D<float4> g_Level1;
[[vk::binding(4, 3)]] [format("rgba16f")] RWTexture2D<float4> g_Level2;
[[vk::binding(5, 3)]] [format("rgba16f")] RWTexture2D<float4> g_Level3;
[[vk::binding(6, 3)]] [format("rgba16f")] RWTexture2D<float4> g_Level4;
[[vk::binding(7, 3)]] [format("rgba16f")] RWTexture2D<float4> g_Level5;
[[vk::binding(8, 3)]] [format("rgba16f")] RWTexture2D<float4> g_Level6;
[[vk::binding(9, 3)]] [format("rgba16f")] RWTexture2D<float4> g_Level7;

struct PushConstants
{
    uint2 RenderExtent; // this frame's valid extent at level 0
    uint2 AllocBase;    // the chain's allocated level-0 extent
    uint  FirstLevel;   // the tail's first level (>= 1)
    uint  LevelCount;   // levels in the tail (>= 2)
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

groupshared uint2 g_Texels[TAIL_CAPACITY];

void StoreLevel(uint slot, uint2 id, float4 value)
{
    switch (slot)
    {
    case 0: g_Level0[id] = value; break;
    case 1: g_Level1[id] = value; break;
    case 2: g_Level2[id] = value; break;
    case 3: g_Level3[id] = value; break;
    case 4: g_Level4[id] = value; break;
    case 5: g_Level5[id] = value; break;
    case 6: g_Level6[id] = value; break;
    default: g_Level7[id] = value; break;
    }
}

uint2 ExtentAt(uint2 base, uint mip)
{
    return max(base >> mip, uint2(1, 1));
}

uint2 Pack(float3 c)
{
    return uint2(f32tof16(c.r) | (f32tof16(c.g) << 16), f32tof16(c.b));
}

float3 Unpack(uint2 v)
{
    return float3(f16tof32(v.x & 0xFFFFu), f16tof32(v.x >> 16), f16tof32(v.y & 0xFFFFu));
}

uint OffsetOf(uint index)
{
    uint offset = 0;
    for (uint i = 0; i < index; i++)
    {
        uint2 v = ExtentAt(g_PC.RenderExtent, g_PC.FirstLevel + i);
        offset += v.x * v.y;
    }
    return offset;
}

float3 LoadShared(uint offset, uint2 valid, int2 p)
{
    p = clamp(p, int2(0, 0), int2(valid) - 1);
    return Unpack(g_Texels[offset + uint(p.y) * valid.x + uint(p.x)]);
}

// A bilinear, clamp-to-edge fetch of a shared level at an allocation UV, as the chain sampler
// fetches the level's mip. The caller's clamp to the valid region keeps the footprint inside it.
float3 SampleShared(uint offset, uint2 valid, uint2 alloc, float2 uv)
{
    float2 p = uv * float2(alloc) - 0.5;
    float2 base = floor(p);
    float2 t = p - base;
    int2 i = int2(base);
    float3 a = LoadShared(offset, valid, i);
    float3 b = LoadShared(offset, valid, i + int2(1, 0));
    float3 c = LoadShared(offset, valid, i + int2(0, 1));
    float3 d = LoadShared(offset, valid, i + int2(1, 1));
    return lerp(lerp(a, b, t.x), lerp(c, d, t.x), t.y);
}

[shader("compute")]
[numthreads(THREADS, 1, 1)]
void csMain(uint3 tid : SV_GroupThreadID)
{
    for (uint index = 0; index < g_PC.LevelCount; index++)
    {
        uint mip = g_PC.FirstLevel + index;
        uint2 valid = ExtentAt(g_PC.RenderExtent, mip);
        uint2 parentValid = ExtentAt(g_PC.RenderExtent, mip - 1);
        uint2 parentAlloc = ExtentAt(g_PC.AllocBase, mip - 1);
        float2 scaleUV = float2(parentValid) / float2(parentAlloc);
        float2 maxUV = (float2(parentValid) - 0.5) / float2(parentAlloc);
        float2 step = 1.0 / float2(parentAlloc);
        uint offset = OffsetOf(index);
        uint parentOffset = index == 0 ? 0 : OffsetOf(index - 1);

        uint count = valid.x * valid.y;
        for (uint t = tid.x; t < count; t += THREADS)
        {
            uint2 id = uint2(t % valid.x, t / valid.x);
            float2 centre = ((float2(id) + 0.5) / float2(valid)) * scaleUV;
            float3 sum = float3(0.0, 0.0, 0.0);
            [unroll]
            for (int y = -1; y <= 1; y += 2)
            {
                [unroll]
                for (int x = -1; x <= 1; x += 2)
                {
                    float2 uv = min(centre + float2(x, y) * step, maxUV);
                    sum += index == 0 ? g_Source.SampleLevel(g_Sampler, uv, 0.0).rgb
                                      : SampleShared(parentOffset, parentValid, parentAlloc, uv);
                }
            }
            g_Texels[offset + t] = Pack(sum * 0.25);
        }
        GroupMemoryBarrierWithGroupSync();
    }

    // Every tail level to its mip: the valid sub-rect from shared memory, the rest of the
    // allocation the clear the per-level pass leaves.
    for (uint slot = 0; slot < g_PC.LevelCount; slot++)
    {
        uint mip = g_PC.FirstLevel + slot;
        uint2 valid = ExtentAt(g_PC.RenderExtent, mip);
        uint2 alloc = ExtentAt(g_PC.AllocBase, mip);
        uint offset = OffsetOf(slot);
        uint count = alloc.x * alloc.y;
        for (uint t = tid.x; t < count; t += THREADS)
        {
            uint2 id = uint2(t % alloc.x, t / alloc.x);
            float4 value = float4(0.0, 0.0, 0.0, 1.0);
            if (all(id < valid))
            {
                value = float4(Unpack(g_Texels[offset + id.y * valid.x + id.x]), 1.0);
            }
            StoreLevel(slot, id, value);
        }
    }
}
