// The anamorphic streak's row filter: each invocation owns one row of the streak buffer and replaces
// every texel with Σ_d ratio^|d| · source[x − d], normalised to keep each channel's light, as one
// forward and one backward recursive sweep — an exact exponential falloff of any reach, symmetric
// and falling monotonically from each source. Past the row's ends is zero, so light leaves at the
// frame's edges. Both sweeps run as running averages (each sum times 1 − ratio), so neither exceeds
// the row's brightest texel; that is what lets the forward one ride the half-float destination
// between the sweeps. Mirrors StreakFilterRow (StreakKernel.h).
//
// Set 3 binds the source image, an unused sampler, and the destination (the bloom's down/up set
// layout).

struct PushConstants
{
    uint2  Extent; // the valid extent, in texels
    uint2  Padding;
    float4 Ratio;  // each channel's weight ratio between neighbouring texels; w unused
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

[[vk::binding(0, 3)]] Texture2D<float4> g_Source;
[[vk::binding(1, 3)]] SamplerState g_Sampler;
[[vk::binding(2, 3)]] [format("rgba16f")] RWTexture2D<float4> g_Dest;

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    const int row = int(id.x);
    if (row >= int(g_PC.Extent.y))
    {
        return;
    }
    const int width = int(g_PC.Extent.x);
    const float3 ratio = g_PC.Ratio.xyz;
    const float3 gain = 1.0 - ratio;

    float3 forward = float3(0.0, 0.0, 0.0);
    for (int x = 0; x < width; x++)
    {
        forward = gain * g_Source.Load(int3(x, row, 0)).rgb + ratio * forward;
        g_Dest[int2(x, row)] = float4(forward, 1.0);
    }

    float3 backward = float3(0.0, 0.0, 0.0);
    for (int x = width - 1; x >= 0; x--)
    {
        const float3 source = g_Source.Load(int3(x, row, 0)).rgb;
        backward = gain * source + ratio * backward;
        const float3 sum = g_Dest[int2(x, row)].rgb + backward - gain * source;
        g_Dest[int2(x, row)] = float4(sum / (1.0 + ratio), 1.0);
    }
}
