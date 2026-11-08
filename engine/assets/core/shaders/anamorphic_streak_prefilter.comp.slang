// The anamorphic streak's prefilter: the scene colour's bright pass into the streak buffer, a
// quarter of the frame's width and half its height. Each destination texel averages the 4x2 block
// of scene texels it covers through two bilinear taps, each tap conditioned on its own and the
// average soft-kneed exactly as the bloom's level 0 is (bright_tap.slang), so a source streaks with
// the light it blooms with.
//
// Set 3 binds the scene colour, a clamp-to-edge linear sampler, and the destination (the bloom's
// down/up set layout); sets 0-2 are the typed bindless registries.
#include "bright_tap.slang"
#include "Veng/binding.slang"

struct PushConstants
{
    uint2  DestExtent;     // the destination's valid extent, in texels
    float2 SourceScaleUV;  // the scene colour's validExtent/allocExtent
    float2 SourceMaxUV;    // the scene colour's (validExtent-0.5)/allocExtent
    float  Threshold;      // the bright pass's knee, in scene units
    float  FireflyCeiling; // the most luminance one tap carries, in scene units
};

VE_PUSH_CONSTANTS(PushConstants, g_PC);

struct AnamorphicStreakPrefilterSet3
{
    Texture2D<float4> Source;
    SamplerState Sampler;
    [format("rgba16f")] RWTexture2D<float4> Dest;
};
VE_DESCRIPTOR_SET(3, AnamorphicStreakPrefilterSet3, g_Set3);

float3 SampleSource(float2 uv)
{
    const float2 physical = min(uv * g_PC.SourceScaleUV, g_PC.SourceMaxUV);
    return g_Set3.Source.SampleLevel(g_Set3.Sampler, physical, 0.0).rgb;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_PC.DestExtent.x || id.y >= g_PC.DestExtent.y)
    {
        return;
    }
    const float2 texel = 1.0 / float2(g_PC.DestExtent);
    const float2 uv = (float2(id.xy) + 0.5) * texel;
    // A quarter of a destination texel either side is the centre of each 2x2 scene block.
    const float2 offset = float2(0.25 * texel.x, 0.0);
    const float3 left = ConditionBrightTap(SampleSource(uv - offset), g_PC.FireflyCeiling);
    const float3 right = ConditionBrightTap(SampleSource(uv + offset), g_PC.FireflyCeiling);
    const float3 average = 0.5 * (left + right);
    g_Set3.Dest[int2(id.xy)] = float4(average * BrightPassGain(average, g_PC.Threshold), 1.0);
}
