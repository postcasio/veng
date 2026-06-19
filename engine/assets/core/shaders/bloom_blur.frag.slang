// Bloom separable-Gaussian blur fragment stage: one axis of a single-level
// separable Gaussian. It samples its input target (the bright-pass residual for the
// horizontal axis, the horizontal result for the vertical axis) through the bindless
// set-0 array (a runtime-bound handle field the post pass rewrites each frame) and
// convolves a nine-tap Gaussian along one axis, selected by the Horizontal param the
// renderer sets per stage. Two invocations of this one stage — horizontal then
// vertical — compose the full 2D blur at the cost of 2*N taps instead of N*N.
//
// The texel step is derived from the input's dimensions (GetDimensions), so the
// blur radius in pixels is resolution-independent.

struct MaterialParams
{
    uint Source;         // bindless sampled-image index (runtime-bound each frame)
    uint SourceSampler;  // bindless sampler index (runtime-bound each frame)
    uint Horizontal;     // 1 = blur along X, 0 = blur along Y (renderer-set per stage)
};

static const uint MaterialParamStride = 256;

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];
[[vk::binding(4, 0)]] ByteAddressBuffer g_MaterialParams;

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

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

// A nine-tap (center + four each side) normalized Gaussian.
static const float Weights[5] = { 0.227027, 0.194595, 0.121622, 0.054054, 0.016216 };

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(g_PC.MaterialIndex);

    Texture2D source = g_Textures[NonUniformResourceIndex(p.Source)];
    SamplerState samp = g_Samplers[NonUniformResourceIndex(p.SourceSampler)];

    uint width, height;
    source.GetDimensions(width, height);
    float2 texel = 1.0 / float2(width, height);

    float2 axis = (p.Horizontal != 0u) ? float2(texel.x, 0.0) : float2(0.0, texel.y);

    float3 result = source.Sample(samp, input.v_UV).rgb * Weights[0];
    for (uint i = 1u; i < 5u; ++i)
    {
        float2 offset = axis * float(i);
        result += source.Sample(samp, input.v_UV + offset).rgb * Weights[i];
        result += source.Sample(samp, input.v_UV - offset).rgb * Weights[i];
    }

    return float4(result, 1.0);
}
