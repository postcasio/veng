// Auto-exposure metering: builds a log-luminance histogram of the lit HDR target into the
// renderer's ring-buffered readback buffer. Each thread covers one texel of the (dynamic-
// resolution) valid region, takes its Rec.709 luminance, and bins it: texels darker than the
// metering floor fall into bin 0 (the "black bin", excluded from the average), the rest scatter
// across bins 1..255 spanning [MinLogLum, MaxLogLum] in log2 space. A per-workgroup groupshared
// histogram's non-empty bins are atomically merged into the frame's global region. The renderer
// reads the histogram one frame later, averages bins 1..255 (ignoring the black bin) and eases the
// tonemap exposure toward that value — so a predominantly-black scene with sparse highlights
// meters on its lit content instead of collapsing to the floor. A texel whose surface carries
// SurfaceFlagExcludedFromMetering in the g-buffer is not counted at all. This pass only measures;
// it applies nothing.
#include "Veng/surface_flags.slang"
#include "Veng/binding.slang"

struct AutoExposureSet3
{
    Texture2D<float4> Hdr;
    SamplerState Sampler;
    RWStructuredBuffer<uint> Histogram;
    Texture2D<float4> Orm;
};
VE_DESCRIPTOR_SET(3, AutoExposureSet3, g_Set3);

struct PushConstants
{
    float2 SourceScaleUV; // HDR validExtent/allocExtent (the dynamic-resolution sub-rect)
    float2 SourceMaxUV;   // (validExtent-0.5)/allocExtent (bilinear-tap clamp)
    uint2  ValidExtent;   // valid region in texels; threads beyond it contribute nothing
    float2 OrmScaleUV;    // the g-buffer's rendered sub-rect over its own allocation
    float  MinLogLum;     // log2 lower bound; luminance below it falls in the excluded black bin
    float  MaxLogLum;     // log2 upper bound of bin 255
    uint   HistogramBase; // first element of this frame's ring region (bin 0)
    uint   ReadsFlags;    // 0 when no g-buffer is bound (g_Set3.Orm then stands in for nothing)
};

VE_PUSH_CONSTANTS(PushConstants, g_PC);

static const uint GroupDim = 16;
static const uint BinCount = GroupDim * GroupDim; // 256 bins == threads per group

groupshared uint s_Hist[BinCount];

// Map an HDR luminance to a histogram bin: 0 for sub-floor (black) texels, else 1..255 spread
// linearly over [MinLogLum, MaxLogLum] in log2 space.
uint LuminanceBin(float lum)
{
    if (lum < 1e-5)
    {
        return 0;
    }
    float logLum = log2(lum);
    if (logLum < g_PC.MinLogLum)
    {
        return 0;
    }
    float t = saturate((logLum - g_PC.MinLogLum) / max(g_PC.MaxLogLum - g_PC.MinLogLum, 1e-5));
    return clamp(uint(t * 254.0 + 1.0), 1u, 255u);
}

[shader("compute")]
[numthreads(GroupDim, GroupDim, 1)]
void csMain(uint3 gid : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
    // Zero this workgroup's shared histogram (one bin per thread) before any accumulation.
    s_Hist[groupIndex] = 0;
    GroupMemoryBarrierWithGroupSync();

    if (gid.x < g_PC.ValidExtent.x && gid.y < g_PC.ValidExtent.y)
    {
        // Pixel-center UV over the valid region, mapped into the HDR's valid sub-rect.
        float2 uv = (float2(gid.xy) + 0.5) / float2(g_PC.ValidExtent);
        // The scene colour may be at the resolved resolution and the g-buffer at the rendered one,
        // so the flag is read at the same place in the frame, mapped into the g-buffer's sub-rect.
        bool metered = g_PC.ReadsFlags == 0u ||
                       !GBufferExcludedFromMetering(LoadSurfaceFlags(g_Set3.Orm, uv * g_PC.OrmScaleUV));
        if (metered)
        {
            float2 src = min(uv * g_PC.SourceScaleUV, g_PC.SourceMaxUV);
            float3 hdr = g_Set3.Hdr.SampleLevel(g_Set3.Sampler, src, 0.0).rgb;
            float lum = dot(hdr, float3(0.2126, 0.7152, 0.0722));
            InterlockedAdd(s_Hist[LuminanceBin(lum)], 1u);
        }
    }

    GroupMemoryBarrierWithGroupSync();

    // Merge this workgroup's bins into the frame's global ring region. A group touches few of
    // the 256 bins, so an empty bin costs no global atomic.
    const uint count = s_Hist[groupIndex];
    if (count != 0u)
    {
        InterlockedAdd(g_Set3.Histogram[g_PC.HistogramBase + groupIndex], count);
    }
}
