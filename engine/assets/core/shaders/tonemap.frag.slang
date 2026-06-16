// Tonemap fragment stage: samples the HDR lighting target through the bindless
// set-0 array, applies an exposure scale, maps the linear HDR color to the
// display range with the ACES filmic approximation, and writes it to the output.
// The terminal stage of the deferred chain — it replaces a plain clamp with a
// curve that rolls off highlights instead of hard-clipping them.
//
// The curve is the Narkowicz ACES approximation (a single rational fit of the
// reference ACES tone curve): cheap, no LUT, perceptually close enough for v1.
// The output stays linear; the swapchain composite handles final display
// encoding.
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint HdrTexture;
    uint Sampler;
    float Exposure;
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
float4 fsMain(VSOutput input) : SV_Target
{
    float3 hdr = g_Textures[NonUniformResourceIndex(g_PC.HdrTexture)].Sample(
        g_Samplers[NonUniformResourceIndex(g_PC.Sampler)], input.v_UV).rgb;
    float3 tonemapped = AcesApprox(hdr * g_PC.Exposure);
    return float4(tonemapped, 1.0);
}
