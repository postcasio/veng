// SSR reflection blur downsample: one destination mip per dispatch, building the chain the SSR
// composite reads a rough reflection from. Each invocation owns one destination texel and
// low-passes the source mip (the trace's sharp mip 0 for level 1, the prior level otherwise)
// through the 13-tap Call-of-Duty/Jimenez kernel at source-texel offsets: five overlapping 2x2
// box groups read bilinearly, the inner one weighted 0.5 and the four corner ones 0.125. Its six
// source texels of support band-limit each level before the 2:1 decimation, so a small bright
// reflection spreads smoothly down the chain instead of stepping between coarse texels. In one
// axis the kernel's variance is 1.75 source texels squared.
//
// Both reflection targets are premultiplied by the trace's confidence and filtered by the same
// weights: the radiance with the confidence in alpha, and the hit distance. A coarse level
// therefore fades where few rays hit, and its distance divided by its alpha is the
// confidence-weighted mean hit distance over its footprint.
//
// The surface chain — the device depth of the surface each texel's ray left from and its square,
// each times its coverage, and the coverage — is filtered by the same kernel, so over its coverage
// a coarse texel holds the mean and variance of the surfaces its reflection averaged. On a plane
// the mean is a point on the plane, since device depth is affine in screen position across one,
// and the variance is the plane's own gradient across the kernel. Thirty-two-bit float is not
// guaranteed filterable, so its bilinear taps are taken by hand from four loads each.
//
// Set 0 is reserved for the bindless registry; this closed producer->consumer chain binds its own
// resources on set 3: each target's source mip and storage destination mip, and one clamp-to-edge
// linear sampler.
[[vk::binding(0, 3)]] Texture2D<float4> g_Source;
[[vk::binding(1, 3)]] SamplerState g_Sampler;
[[vk::binding(2, 3)]] [format("rgba16f")] RWTexture2D<float4> g_Dest;
[[vk::binding(3, 3)]] Texture2D<float> g_DistanceSource;
[[vk::binding(4, 3)]] [format("r16f")] RWTexture2D<float> g_DistanceDest;
[[vk::binding(5, 3)]] Texture2D<float4> g_SurfaceSource;
[[vk::binding(6, 3)]] [format("rgba32f")] RWTexture2D<float4> g_SurfaceDest;

struct PushConstants
{
    uint2 DestExtent;  // destination mip dimensions in texels
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

// The kernel's thirteen tap offsets in source texels: the outer 3x3 grid in row order, then the
// inner ring.
static const float2 TapOffsets[13] = {
    float2(-2, -2), float2(0, -2), float2(2, -2),
    float2(-2, 0),  float2(0, 0),  float2(2, 0),
    float2(-2, 2),  float2(0, 2),  float2(2, 2),
    float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, 1),
};

// Each tap's weight: every outer tap is shared by the corner groups it falls in (0.125 / 4 each),
// the inner ring is the 0.5 group.
static const float TapWeights[13] = {
    0.03125, 0.0625, 0.03125,
    0.0625,  0.125,  0.0625,
    0.03125, 0.0625, 0.03125,
    0.125,   0.125,  0.125,   0.125,
};

// A clamp-to-edge bilinear tap of the surface chain at source texel coordinate `coord`.
float4 LoadSurfaceBilinear(float2 coord, int2 size)
{
    float2 position = coord - 0.5;
    float2 base = floor(position);
    float2 f = position - base;
    int2 low = clamp(int2(base), int2(0, 0), size - 1);
    int2 high = clamp(int2(base) + 1, int2(0, 0), size - 1);
    float4 top = lerp(g_SurfaceSource.Load(int3(low.x, low.y, 0)),
                      g_SurfaceSource.Load(int3(high.x, low.y, 0)), f.x);
    float4 bottom = lerp(g_SurfaceSource.Load(int3(low.x, high.y, 0)),
                         g_SurfaceSource.Load(int3(high.x, high.y, 0)), f.x);
    return lerp(top, bottom, f.y);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_PC.DestExtent.x || id.y >= g_PC.DestExtent.y)
        return;

    uint sourceWidth;
    uint sourceHeight;
    g_Source.GetDimensions(sourceWidth, sourceHeight);
    float2 sourceTexel = 1.0 / float2(sourceWidth, sourceHeight);
    float2 uv = (float2(id.xy) + 0.5) / float2(g_PC.DestExtent);

    float4 colour = float4(0.0, 0.0, 0.0, 0.0);
    float distance = 0.0;
    float4 surface = float4(0.0, 0.0, 0.0, 0.0);
    int2 sourceSize = int2(sourceWidth, sourceHeight);
    [unroll]
    for (uint tap = 0; tap < 13; ++tap)
    {
        float2 tapUV = uv + TapOffsets[tap] * sourceTexel;
        colour += g_Source.SampleLevel(g_Sampler, tapUV, 0.0) * TapWeights[tap];
        distance += g_DistanceSource.SampleLevel(g_Sampler, tapUV, 0.0) * TapWeights[tap];
        surface += LoadSurfaceBilinear(tapUV * float2(sourceSize), sourceSize) * TapWeights[tap];
    }

    g_Dest[int2(id.xy)] = colour;
    g_DistanceDest[int2(id.xy)] = distance;
    g_SurfaceDest[int2(id.xy)] = surface;
}
