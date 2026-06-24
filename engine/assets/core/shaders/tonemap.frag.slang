// Tonemap fragment stage: the engine's first PostProcess-domain material. It
// samples the HDR lighting target through the bindless set-0 array (the HDR
// input is a runtime-bound handle field the post pass rewrites each frame),
// applies an exposure scale, maps the linear HDR color to the display range with
// the ACES filmic approximation, and writes a single SV_Target0 (the PostProcess
// output contract). The terminal stage of the deferred chain — it replaces a
// plain clamp with a curve that rolls off highlights instead of hard-clipping
// them.
//
// The curve is the Narkowicz ACES approximation (a single rational fit of the
// reference ACES tone curve): cheap, no LUT, perceptually close enough. The
// output stays linear; the swapchain composite handles final display encoding.

// MaterialParams is this material's whole parameter block: the bindless handle
// slots for the HDR input (uint members the post pass writes each frame with the
// renderer's current HDR target index) beside the authored Exposure param,
// padded to MaterialParamStride. Read through the standard set-0 byte-addressed
// material buffer at index * MaterialParamStride.
struct MaterialParams
{
    // RenderScale is first so its offset is 0 in both the cooker's std140 reflection and the
    // shader's scalar-layout Load: a float4 placed after the scalar fields would land at offset 16
    // under std140 but 12 under scalar, so the runtime would write it where the shader cannot read.
    float4 RenderScale; // dynamic-resolution upscale: xy source validExtent/allocExtent, zw clamp
    uint   Hdr;         // bindless sampled-image index (runtime-bound each frame)
    uint   HdrSampler;  // bindless sampler index (runtime-bound each frame)
    float  Exposure;    // authored param, set per frame from the SceneView
};

static const uint MaterialParamStride = 256;

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];
[[vk::binding(4, 0)]] ByteAddressBuffer g_MaterialParams;

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

// A PostProcess shader's per-draw selector sits at push-constant offset 0 (no
// geometry block precedes it, unlike a Surface shader's MVP).
struct PushConstants
{
    uint MaterialIndex;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

// The Narkowicz ACES filmic approximation: a rational fit of the ACES reference
// tone curve, applied per channel.
float3 AcesApprox(float3 x)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(g_PC.MaterialIndex);
    // The HDR source is rendered into a dynamic-resolution sub-rect; this terminal pass renders the
    // full output and upscales by mapping the full-output UV across the source's valid region
    // (RenderScale.xy), clamped (RenderScale.zw) so the bilinear tap never bleeds past it. At scale
    // 1.0 RenderScale is (1,1,~1,~1) and this is the plain full-resolution sample.
    float2 uv = min(input.v_UV * p.RenderScale.xy, p.RenderScale.zw);
    float3 hdr = g_Textures[NonUniformResourceIndex(p.Hdr)].Sample(
        g_Samplers[NonUniformResourceIndex(p.HdrSampler)], uv).rgb;
    float3 tonemapped = AcesApprox(hdr * p.Exposure);
    return float4(tonemapped, 1.0);
}
