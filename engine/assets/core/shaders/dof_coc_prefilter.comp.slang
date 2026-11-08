// Depth-of-field circle-of-confusion + prefilter: the battery's first stage. One invocation
// owns one half-resolution texel. It reconstructs the view-space depth from the depth
// attachment, evaluates the thin-lens circle of confusion from the three defocus constants,
// and splits the scene color into a near layer and a far layer with the layer's own radius in
// alpha — the form the ring gather reads. A third single-tap buffer carries the signed radius
// and the depth for the tile reduction.
//
// The circle-of-confusion expression mirrors ComputeCircleOfConfusion (Veng/Scene/Camera.h)
// exactly: CocScale * Aperture * (depth - FocusDistance) / depth, in full-resolution pixels,
// halved into the half-resolution working space.
//
// Set 0 is reserved for the bindless registry; this closed producer->consumer chain binds its
// own resources on set 1.

#include "Veng/binding.slang"
struct DofCocPrefilterSet3
{
    Texture2D<float4> Color;
    Texture2D<float> Depth;
    SamplerState Sampler;
    [format("rgba16f")] RWTexture2D<float4> Near;
    [format("rgba16f")] RWTexture2D<float4> Far;
    [format("rg16f")] RWTexture2D<float2> Coc;
};
VE_DESCRIPTOR_SET(3, DofCocPrefilterSet3, g_Set3);

struct PushConstants
{
    uint2 DestExtent;     // half-resolution valid extent in texels
    float2 ScaleUV;       // full-res source validExtent/allocExtent
    float2 MaxUV;         // full-res source (validExtent-0.5)/allocExtent
    float2 DepthParams;   // (Proj[2][2], Proj[3][2]): viewDepth = y / (rawDepth + x)
    float Aperture;       // aperture diameter in metres
    float FocusDistance;  // focus plane distance in metres
    float CocScale;       // sensor->pixel scale in pixels per metre (full resolution)
    float MaxCoc;         // radius ceiling in half-resolution pixels
};

VE_PUSH_CONSTANTS(PushConstants, g_PC);

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_PC.DestExtent.x || id.y >= g_PC.DestExtent.y)
        return;

    // The half-resolution texel center lands exactly on a full-resolution 2x2 corner, so one
    // linear tap is the 2x2 box downsample.
    float2 logical = (float2(id.xy) + 0.5) / float2(g_PC.DestExtent);
    float2 uv = min(logical * g_PC.ScaleUV, g_PC.MaxUV);

    float3 color = g_Set3.Color.SampleLevel(g_Set3.Sampler, uv, 0.0).rgb;
    float rawDepth = g_Set3.Depth.SampleLevel(g_Set3.Sampler, uv, 0.0).r;
    // DepthParams is filled from the actual (reverse-Z) projection's depth row, so the
    // reconstruction inverts whatever it maps; the guard floors the divisor's *magnitude*
    // rather than clamping it to a sign, staying correct as those terms change sign. The floor
    // is far below any drawn depth: under an infinite projection x is 0 and a distant surface's
    // device depth is near / viewDepth, so a larger floor would clamp its depth.
    float denom = rawDepth + g_PC.DepthParams.x;
    denom = denom >= 0.0 ? max(denom, 1e-30) : min(denom, -1e-30);
    float viewDepth = g_PC.DepthParams.y / denom;

    float coc = 0.0;
    if (viewDepth > 0.0)
        coc = g_PC.CocScale * g_PC.Aperture * (viewDepth - g_PC.FocusDistance) / viewDepth;

    // Full-resolution pixels into the half-resolution working space, then bounded so the
    // gather's kernel radius stays inside the tile budget.
    float cocHalf = clamp(coc * 0.5, -g_PC.MaxCoc, g_PC.MaxCoc);

    g_Set3.Near[int2(id.xy)] = float4(color, max(-cocHalf, 0.0));
    g_Set3.Far[int2(id.xy)] = float4(color, max(cocHalf, 0.0));
    g_Set3.Coc[int2(id.xy)] = float2(cocHalf, viewDepth);
}
