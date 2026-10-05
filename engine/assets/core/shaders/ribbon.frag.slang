// Ribbon fragment stage. Shapes the record's footprint — full on a band's centre line or a dot's
// centre, falling off as (1 - r^2)^2 to nothing at its rim — and returns the straight colour with
// that coverage times the opacity. fsMain returns the colour alone; fsMainMasked also returns the
// bloom-mask amplitude, the luminance of the light the fragment contributes, so an HDR ribbon glows
// regardless of the bloom threshold. The alpha pipeline composites it straight-alpha over and the
// additive pipeline adds it weighted by coverage, so one stage serves both.
//
// The post-resolve entries draw where no depth buffer is attached, so they occlude by hand: the
// render-allocation g-buffer depth is sampled at this pixel through the sub-rect remap, turned back
// into a view depth through the projection's depth rows (which TAA's jitter leaves alone), and a
// fragment farther than it is discarded — the scene placement's depth test, taken per fragment.
#include "view_constants.slang"
#include "ribbon_common.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct MaskedOutput
{
    float4 Color : SV_Target0;
    float Mask : SV_Target1;
};

static const float3 LumaWeights = float3(0.2126, 0.7152, 0.0722);

// How much farther than the scene surface, as a fraction of its view depth, a post-resolve fragment
// may lie and still draw: absorbs the sampled depth's precision so linework on a surface survives.
static const float OcclusionTolerance = 1e-3;

float4 ShadeRibbon(VSOutput input)
{
    float edge = saturate(1.0 - dot(input.v_Profile, input.v_Profile));
    return float4(input.v_Color.rgb, input.v_Color.a * edge * edge);
}

float MaskOf(float4 color)
{
    return saturate(dot(color.rgb * color.a, LumaWeights));
}

// The view depth a reverse-Z device depth stands for, through any projection whose clip z and w
// are affine in view z.
float ViewDepthFromDevice(float4x4 proj, float device)
{
    float z = (proj[2][3] - device * proj[3][3]) / (device * proj[3][2] - proj[2][2]);
    return -z;
}

void DiscardOccluded(VSOutput input)
{
    if (g_PC.Occluded == 0u)
    {
        return;
    }
    ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);
    float2 logicalUV = input.sv_position.xy / g_PC.TargetExtent;
    float2 sampleUV = ScaledSampleUV(logicalUV, view.RenderScaleUV.xy, view.MaxValidUV.xy);
    // The indices are per-draw uniform push constants, so they cannot diverge.
    float device = g_Textures[g_PC.DepthTexture].Sample(g_Samplers[g_PC.Sampler], sampleUV).r;
    // Reverse-Z clears to 0: nothing was drawn at this pixel.
    if (device <= 0.0)
    {
        return;
    }
    if (input.v_ViewDepth > ViewDepthFromDevice(g_PC.Proj, device) * (1.0 + OcclusionTolerance))
    {
        discard;
    }
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    return ShadeRibbon(input);
}

[shader("fragment")]
MaskedOutput fsMainMasked(VSOutput input)
{
    MaskedOutput output;
    output.Color = ShadeRibbon(input);
    output.Mask = MaskOf(output.Color);
    return output;
}

[shader("fragment")]
float4 fsMainPostResolve(VSOutput input) : SV_Target
{
    DiscardOccluded(input);
    return ShadeRibbon(input);
}

[shader("fragment")]
MaskedOutput fsMainPostResolveMasked(VSOutput input)
{
    DiscardOccluded(input);
    MaskedOutput output;
    output.Color = ShadeRibbon(input);
    output.Mask = MaskOf(output.Color);
    return output;
}
