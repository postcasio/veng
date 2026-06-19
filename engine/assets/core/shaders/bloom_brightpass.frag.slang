// Bloom bright-pass fragment stage: the first stage of the bloom post chain. It
// samples the HDR lighting target through the bindless set-0 array (a runtime-bound
// handle field the post pass rewrites each frame), keeps only the luminance above a
// soft Threshold knee, and writes that bright residual to a single SV_Target0 (the
// PostProcess output contract). The result feeds the separable blur stages.
//
// The knee is a smooth quadratic soft-threshold (a curve, not a hard step) so a
// surface hovering near Threshold ramps in gradually rather than popping — the
// standard bright-pass shape that avoids a hard luminance edge in the bloom.

// MaterialParams is this material's whole parameter block: the bindless handle
// slots for the HDR input (uint members the post pass writes each frame) beside the
// authored Threshold param, padded to MaterialParamStride. Read through the standard
// set-0 byte-addressed material buffer at index * MaterialParamStride.
struct MaterialParams
{
    uint  Hdr;        // bindless sampled-image index (runtime-bound each frame)
    uint  HdrSampler; // bindless sampler index (runtime-bound each frame)
    float Threshold;  // authored param: the luminance knee
};

static const uint MaterialParamStride = 256;

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];
[[vk::binding(4, 0)]] ByteAddressBuffer g_MaterialParams;

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

// A PostProcess shader's per-draw selector sits at push-constant offset 0.
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

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(g_PC.MaterialIndex);

    float3 hdr = g_Textures[NonUniformResourceIndex(p.Hdr)].Sample(
        g_Samplers[NonUniformResourceIndex(p.HdrSampler)], input.v_UV).rgb;

    // Rec. 709 relative luminance — the perceptual weight the knee runs against.
    float luma = dot(hdr, float3(0.2126, 0.7152, 0.0722));

    // Smooth quadratic knee: a soft ramp over a Threshold-wide band centered on
    // Threshold, so contribution rises continuously from 0 instead of stepping.
    float knee = max(p.Threshold, 1e-4);
    float soft = clamp((luma - knee) / knee, 0.0, 1.0);
    float weight = soft * soft;

    return float4(hdr * weight, 1.0);
}
