// Auto-exposure metering: one workgroup reduces the lit HDR target to a single average
// log-luminance value, written into the renderer's ring-buffered readback buffer. Each of the
// 16x16 threads samples one cell of a regular grid over the (dynamic-resolution) valid region,
// takes its Rec.709 luminance, clamps the log to [MinLogLum, MaxLogLum] so a stray highlight or
// black pixel cannot drag the average, and the group tree-reduces to the mean. The renderer reads
// the value one frame later and eases the tonemap exposure toward it (the temporal eye
// adaptation), so this pass only measures — it applies nothing.
[[vk::binding(0, 1)]] Texture2D<float4> g_Hdr;
[[vk::binding(1, 1)]] SamplerState g_Sampler;
[[vk::binding(2, 1)]] RWStructuredBuffer<float> g_Result;

struct PushConstants
{
    float2 SourceScaleUV; // HDR validExtent/allocExtent (the dynamic-resolution sub-rect)
    float2 SourceMaxUV;   // (validExtent-0.5)/allocExtent (bilinear-tap clamp)
    float  MinLogLum;     // lower clamp on a sample's log2 luminance
    float  MaxLogLum;     // upper clamp on a sample's log2 luminance
    uint   ResultIndex;   // element in the ring buffer this frame writes
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

static const uint GroupDim = 16;
static const uint SampleCount = GroupDim * GroupDim;

groupshared float s_LogLum[SampleCount];

[shader("compute")]
[numthreads(GroupDim, GroupDim, 1)]
void csMain(uint3 tid : SV_GroupThreadID)
{
    uint local = tid.y * GroupDim + tid.x;

    // Sample the cell center of a GroupDim x GroupDim grid over the valid region.
    float2 uv = (float2(tid.xy) + 0.5) / float2(GroupDim, GroupDim);
    float2 src = min(uv * g_PC.SourceScaleUV, g_PC.SourceMaxUV);
    float3 hdr = g_Hdr.SampleLevel(g_Sampler, src, 0.0).rgb;

    float lum = dot(hdr, float3(0.2126, 0.7152, 0.0722));
    float logLum = clamp(log2(max(lum, 1e-5)), g_PC.MinLogLum, g_PC.MaxLogLum);
    s_LogLum[local] = logLum;

    GroupMemoryBarrierWithGroupSync();

    // Tree reduction to the sum, then the mean in element 0.
    for (uint stride = SampleCount / 2; stride > 0; stride >>= 1)
    {
        if (local < stride)
        {
            s_LogLum[local] += s_LogLum[local + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (local == 0)
    {
        g_Result[g_PC.ResultIndex] = s_LogLum[0] / float(SampleCount);
    }
}
