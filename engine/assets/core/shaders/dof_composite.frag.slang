// Depth-of-field composite: blends the two half-resolution blurred layers over the full-
// resolution HDR by each layer's coverage and writes the HDR intermediate the bloom/tonemap tail
// reads. The far layer goes down first (it sits behind the focus plane) and the near layer over
// it, so a defocused foreground covers both the sharp scene and the blurred background.
//
// A coverage of zero leaves the source untouched, so an in-focus texel is the HDR value it was.
// The composite runs ahead of bloom on purpose: a defocused highlight must still bloom.
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint HdrTexture;      // full-resolution lit HDR (the base the layers cover)
    uint NearTexture;     // near-field gathered + filled layer
    uint FarTexture;      // far-field gathered + filled layer
    uint Sampler;         // shared linear clamp sampler
    float2 ScaleUV;       // full-res source validExtent/allocExtent
    float2 MaxUV;         // full-res source (validExtent-0.5)/allocExtent
    float2 HalfScaleUV;   // half-res layer validExtent/allocExtent
    float2 HalfMaxUV;     // half-res layer (validExtent-0.5)/allocExtent
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    SamplerState samp = g_Samplers[NonUniformResourceIndex(g_PC.Sampler)];

    float2 uvFull = min(input.v_UV * g_PC.ScaleUV, g_PC.MaxUV);
    float2 uvHalf = min(input.v_UV * g_PC.HalfScaleUV, g_PC.HalfMaxUV);

    float3 base = g_Textures[NonUniformResourceIndex(g_PC.HdrTexture)].Sample(samp, uvFull).rgb;
    float4 far = g_Textures[NonUniformResourceIndex(g_PC.FarTexture)].Sample(samp, uvHalf);
    float4 near = g_Textures[NonUniformResourceIndex(g_PC.NearTexture)].Sample(samp, uvHalf);

    float3 color = lerp(base, far.rgb, saturate(far.a));
    color = lerp(color, near.rgb, saturate(near.a));
    return float4(color, 1.0);
}
