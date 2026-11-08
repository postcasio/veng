// The anamorphic streak's row filter: each workgroup owns one row of the streak buffer and replaces
// every texel with Σ_d ratio^|d| · source[x − d], normalised to keep each channel's light, as one
// forward and one backward recursive sweep — an exact exponential falloff of any reach, symmetric
// and falling monotonically from each source. Past the row's ends is zero, so light leaves at the
// frame's edges. Both sweeps run as running averages (each sum times 1 − ratio), so neither exceeds
// the row's brightest texel; that is what lets the forward one ride the half-float destination
// between the sweeps. Mirrors StreakFilterRow (StreakKernel.h).
//
// A sweep is the first-order recurrence y = ratio·y + gain·x, and the affine maps it composes are
// associative, so the row is cut into one contiguous run per thread: each thread reduces its run to
// one map, a workgroup scan of those maps hands every run the sweep's value entering it, and each
// thread then replays its run from that value. The backward sweep's map over a run is taken in the
// same ascending pass, as Σ gain·ratio^(x − begin)·x.
//
// Set 3 binds the source image, an unused sampler, and the destination (the bloom's down/up set
// layout).

#include "Veng/binding.slang"

struct PushConstants
{
    uint2  Extent; // the valid extent, in texels
    uint2  Padding;
    float4 Ratio;  // each channel's weight ratio between neighbouring texels; w unused
};

VE_PUSH_CONSTANTS(PushConstants, g_PC);

struct AnamorphicStreakFilterSet3
{
    Texture2D<float4> Source;
    SamplerState Sampler;
    [format("rgba16f")] RWTexture2D<float4> Dest;
};
VE_DESCRIPTOR_SET(3, AnamorphicStreakFilterSet3, g_Set3);

static const uint GroupSize = 256;

// One affine map per thread, y_out = Scale · y_in + Offset, per channel.
groupshared float3 s_Scale[GroupSize];
groupshared float3 s_Offset[GroupSize];

// Replaces each slot with the composition of every map at or before it in slot order, so slot i
// holds the map taking the sweep's value entering slot 0 to its value leaving slot i.
void InclusiveScan(uint slot)
{
    for (uint offset = 1; offset < GroupSize; offset <<= 1)
    {
        float3 earlierScale = float3(1.0, 1.0, 1.0);
        float3 earlierOffset = float3(0.0, 0.0, 0.0);
        const bool combine = slot >= offset;
        if (combine)
        {
            earlierScale = s_Scale[slot - offset];
            earlierOffset = s_Offset[slot - offset];
        }
        GroupMemoryBarrierWithGroupSync();
        if (combine)
        {
            const float3 scale = s_Scale[slot];
            s_Offset[slot] = scale * earlierOffset + s_Offset[slot];
            s_Scale[slot] = scale * earlierScale;
        }
        GroupMemoryBarrierWithGroupSync();
    }
}

[shader("compute")]
[numthreads(GroupSize, 1, 1)]
void csMain(uint3 group : SV_GroupID, uint thread : SV_GroupIndex)
{
    const int row = int(group.x);
    const int width = int(g_PC.Extent.x);
    const float3 ratio = g_PC.Ratio.xyz;
    const float3 gain = 1.0 - ratio;

    // This thread's run of the row; past the row's end it is empty, the identity map.
    const int run = (width + int(GroupSize) - 1) / int(GroupSize);
    const int begin = min(int(thread) * run, width);
    const int end = min(begin + run, width);

    float3 power = float3(1.0, 1.0, 1.0);
    float3 forward = float3(0.0, 0.0, 0.0);
    float3 backward = float3(0.0, 0.0, 0.0);
    for (int x = begin; x < end; x++)
    {
        const float3 source = gain * g_Set3.Source.Load(int3(x, row, 0)).rgb;
        forward = source + ratio * forward;
        backward += power * source;
        power *= ratio;
    }

    // The forward sweep enters this run with the composition of every run before it.
    s_Scale[thread] = power;
    s_Offset[thread] = forward;
    GroupMemoryBarrierWithGroupSync();
    InclusiveScan(thread);
    const float3 forwardIn = thread > 0 ? s_Offset[thread - 1] : float3(0.0, 0.0, 0.0);
    GroupMemoryBarrierWithGroupSync();

    // The backward sweep enters it with the composition of every run after it, nearest last: the
    // same scan over the runs in reverse.
    const uint reversed = GroupSize - 1 - thread;
    s_Scale[reversed] = power;
    s_Offset[reversed] = backward;
    GroupMemoryBarrierWithGroupSync();
    InclusiveScan(reversed);
    const float3 backwardIn = reversed > 0 ? s_Offset[reversed - 1] : float3(0.0, 0.0, 0.0);

    if (row >= int(g_PC.Extent.y))
    {
        return;
    }

    forward = forwardIn;
    for (int x = begin; x < end; x++)
    {
        forward = gain * g_Set3.Source.Load(int3(x, row, 0)).rgb + ratio * forward;
        g_Set3.Dest[int2(x, row)] = float4(forward, 1.0);
    }

    // The backward sweep reads back what this thread just wrote to Dest. Metal orders a
    // read_write texture's store before a later load only across a texture memory barrier.
    AllMemoryBarrierWithGroupSync();

    backward = backwardIn;
    for (int x = end - 1; x >= begin; x--)
    {
        const float3 source = g_Set3.Source.Load(int3(x, row, 0)).rgb;
        backward = gain * source + ratio * backward;
        const float3 sum = g_Set3.Dest[int2(x, row)].rgb + backward - gain * source;
        g_Set3.Dest[int2(x, row)] = float4(sum / (1.0 + ratio), 1.0);
    }
}
