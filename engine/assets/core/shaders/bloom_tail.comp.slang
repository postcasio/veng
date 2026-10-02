// Bloom coarse tail: the pyramid's smallest levels, down-swept and then up-swept by one workgroup in
// one dispatch. Each of those levels is a handful of texels, so as separate dispatches they are
// almost entirely per-dispatch overhead; here the workgroup computes them level by level in shared
// memory and synchronizes between levels instead.
//
// The tail is levels First .. First+LevelCount-1, the last being the pyramid's coarsest. Level First
// is filtered from level First-1, which an earlier dispatch wrote, through the sampler exactly as
// bloom_down does; every deeper level is filtered from the shared copy of the level above it with
// the same bilinear taps evaluated by hand. The up-sweep then accumulates
// level[k] += tent(level[k+1]) * Radius from the coarsest level back to First, and every tail level's
// final value is written to its storage mip, where the next finer up-step reads level First.
//
// Shared memory holds each level's valid region at the precision its mip stores (RGB as halves, the
// alpha the per-level dispatches write as 1), so a level reads its source exactly as it would off
// the RGBA16F image.
//
// The kernels are bloom_down/bloom_up's COD 13-tap down and 3x3 tent up, or with Kawase set
// bloom_down_kawase/bloom_up_kawase's 5-tap down and 8-tap up; the tail never contains level 0, so
// the bright pass is not here.
#define MAX_TAIL_LEVELS 8
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

struct PushConstants
{
    uint2 ValidBase;  // the pyramid's valid level-0 extent this frame
    uint2 AllocBase;  // the pyramid's allocated level-0 extent
    uint  FirstLevel; // the tail's first level (>= 1)
    uint  LevelCount; // levels in the tail (>= 2)
    float Radius;     // upsample spread scale
    uint  Kawase;     // 1 = the Dual Kawase kernels, 0 = COD
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

// RGB as three halves in two words: the precision an RGBA16F mip stores.
groupshared uint2 g_Texels[TAIL_CAPACITY];

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

// One tail level: where its texels start in shared memory, its valid extent, and the sub-rect map a
// reader samples it through.
struct Level
{
    uint Offset;
    uint2 Valid;
    uint2 Alloc;
    float2 ScaleUV;
    float2 MaxUV;
};

Level LevelAt(uint index)
{
    Level l;
    l.Offset = 0;
    for (uint i = 0; i < index; i++)
    {
        uint2 v = ExtentAt(g_PC.ValidBase, g_PC.FirstLevel + i);
        l.Offset += v.x * v.y;
    }
    l.Valid = ExtentAt(g_PC.ValidBase, g_PC.FirstLevel + index);
    l.Alloc = ExtentAt(g_PC.AllocBase, g_PC.FirstLevel + index);
    l.ScaleUV = float2(l.Valid) / float2(l.Alloc);
    l.MaxUV = (float2(l.Valid) - 0.5) / float2(l.Alloc);
    return l;
}

float3 LoadTexel(Level l, int2 p)
{
    p = clamp(p, int2(0, 0), int2(l.Valid) - 1);
    return Unpack(g_Texels[l.Offset + uint(p.y) * l.Valid.x + uint(p.x)]);
}

// A bilinear, clamp-to-edge fetch of a shared level at a valid-space UV, as the sampler fetches the
// level's mip. Reads never pass the valid region: the clamp to MaxUV keeps the footprint inside it.
float3 SampleShared(Level l, float2 uv)
{
    float2 physical = min(uv * l.ScaleUV, l.MaxUV);
    float2 p = physical * float2(l.Alloc) - 0.5;
    float2 base = floor(p);
    float2 t = p - base;
    int2 i = int2(base);
    float3 a = LoadTexel(l, i);
    float3 b = LoadTexel(l, i + int2(1, 0));
    float3 c = LoadTexel(l, i + int2(0, 1));
    float3 d = LoadTexel(l, i + int2(1, 1));
    return lerp(lerp(a, b, t.x), lerp(c, d, t.x), t.y);
}

// The level before the tail, off the image an earlier dispatch wrote, through the sampler.
float3 SampleSource(float2 uv)
{
    uint mip = g_PC.FirstLevel - 1;
    uint2 valid = ExtentAt(g_PC.ValidBase, mip);
    uint2 alloc = ExtentAt(g_PC.AllocBase, mip);
    float2 physical = min(uv * (float2(valid) / float2(alloc)), (float2(valid) - 0.5) / float2(alloc));
    return g_Source.SampleLevel(g_Sampler, physical, 0.0).rgb;
}

float3 SampleAbove(uint index, float2 uv)
{
    if (index == 0)
    {
        return SampleSource(uv);
    }
    return SampleShared(LevelAt(index - 1), uv);
}

float3 Downsample(uint index, uint2 id, uint2 destExtent)
{
    float2 texel = 1.0 / float2(destExtent);
    float2 uv = (float2(id) + 0.5) * texel;

    if (g_PC.Kawase != 0)
    {
        float2 o = texel * 0.5;
        float3 center = SampleAbove(index, uv);
        float3 tl = SampleAbove(index, uv + float2(-1, -1) * o);
        float3 tr = SampleAbove(index, uv + float2(1, -1) * o);
        float3 bl = SampleAbove(index, uv + float2(-1, 1) * o);
        float3 br = SampleAbove(index, uv + float2(1, 1) * o);
        return center * 0.5 + (tl + tr + bl + br) * 0.125;
    }

    float2 o = texel;
    float3 a = SampleAbove(index, uv + float2(-2, -2) * o);
    float3 b = SampleAbove(index, uv + float2(0, -2) * o);
    float3 c = SampleAbove(index, uv + float2(2, -2) * o);
    float3 d = SampleAbove(index, uv + float2(-2, 0) * o);
    float3 e = SampleAbove(index, uv + float2(0, 0) * o);
    float3 f = SampleAbove(index, uv + float2(2, 0) * o);
    float3 g = SampleAbove(index, uv + float2(-2, 2) * o);
    float3 h = SampleAbove(index, uv + float2(0, 2) * o);
    float3 i = SampleAbove(index, uv + float2(2, 2) * o);
    float3 j = SampleAbove(index, uv + float2(-1, -1) * o);
    float3 k = SampleAbove(index, uv + float2(1, -1) * o);
    float3 l = SampleAbove(index, uv + float2(-1, 1) * o);
    float3 m = SampleAbove(index, uv + float2(1, 1) * o);

    float3 inner = (j + k + l + m) * 0.25;
    float3 tlq = (a + b + d + e) * 0.25;
    float3 trq = (b + c + e + f) * 0.25;
    float3 blq = (d + e + g + h) * 0.25;
    float3 brq = (e + f + h + i) * 0.25;
    return inner * 0.5 + (tlq + trq + blq + brq) * 0.125;
}

float3 Upsample(Level coarser, uint2 id, uint2 destExtent)
{
    float2 texel = 1.0 / float2(destExtent);
    float2 uv = (float2(id) + 0.5) * texel;

    if (g_PC.Kawase != 0)
    {
        float2 h = texel * 0.5;
        float3 sum = SampleShared(coarser, uv + float2(-1, 0) * h) * 2.0;
        sum += SampleShared(coarser, uv + float2(1, 0) * h) * 2.0;
        sum += SampleShared(coarser, uv + float2(0, -1) * h) * 2.0;
        sum += SampleShared(coarser, uv + float2(0, 1) * h) * 2.0;
        sum += SampleShared(coarser, uv + float2(-1, -1) * texel) * 1.0;
        sum += SampleShared(coarser, uv + float2(1, -1) * texel) * 1.0;
        sum += SampleShared(coarser, uv + float2(-1, 1) * texel) * 1.0;
        sum += SampleShared(coarser, uv + float2(1, 1) * texel) * 1.0;
        return sum * (1.0 / 12.0);
    }

    float2 o = texel;
    float3 sum = SampleShared(coarser, uv + float2(-1, -1) * o) * 1.0;
    sum += SampleShared(coarser, uv + float2(0, -1) * o) * 2.0;
    sum += SampleShared(coarser, uv + float2(1, -1) * o) * 1.0;
    sum += SampleShared(coarser, uv + float2(-1, 0) * o) * 2.0;
    sum += SampleShared(coarser, uv + float2(0, 0) * o) * 4.0;
    sum += SampleShared(coarser, uv + float2(1, 0) * o) * 2.0;
    sum += SampleShared(coarser, uv + float2(-1, 1) * o) * 1.0;
    sum += SampleShared(coarser, uv + float2(0, 1) * o) * 2.0;
    sum += SampleShared(coarser, uv + float2(1, 1) * o) * 1.0;
    return sum * (1.0 / 16.0);
}

[shader("compute")]
[numthreads(THREADS, 1, 1)]
void csMain(uint3 tid : SV_GroupThreadID)
{
    // Down-sweep, finest tail level first.
    for (uint index = 0; index < g_PC.LevelCount; index++)
    {
        Level mip = LevelAt(index);
        uint count = mip.Valid.x * mip.Valid.y;
        for (uint t = tid.x; t < count; t += THREADS)
        {
            uint2 id = uint2(t % mip.Valid.x, t / mip.Valid.x);
            g_Texels[mip.Offset + t] = Pack(Downsample(index, id, mip.Valid));
        }
        GroupMemoryBarrierWithGroupSync();
    }

    // Up-sweep, from the level above the coarsest back to the tail's first level. Each texel reads
    // only its own value at the level it writes, so a level updates in place.
    for (uint up = g_PC.LevelCount - 1; up-- > 0;)
    {
        Level mip = LevelAt(up);
        Level coarser = LevelAt(up + 1);
        uint count = mip.Valid.x * mip.Valid.y;
        for (uint t = tid.x; t < count; t += THREADS)
        {
            uint2 id = uint2(t % mip.Valid.x, t / mip.Valid.x);
            float3 existing = Unpack(g_Texels[mip.Offset + t]);
            g_Texels[mip.Offset + t] =
                Pack(existing + Upsample(coarser, id, mip.Valid) * g_PC.Radius);
        }
        GroupMemoryBarrierWithGroupSync();
    }

    // Every tail level's final value to its mip.
    for (uint slot = 0; slot < g_PC.LevelCount; slot++)
    {
        Level mip = LevelAt(slot);
        uint count = mip.Valid.x * mip.Valid.y;
        for (uint t = tid.x; t < count; t += THREADS)
        {
            uint2 id = uint2(t % mip.Valid.x, t / mip.Valid.x);
            StoreLevel(slot, id, float4(Unpack(g_Texels[mip.Offset + t]), 1.0));
        }
    }
}
