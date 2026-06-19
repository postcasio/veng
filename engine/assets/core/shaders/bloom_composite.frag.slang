// Bloom composite fragment stage: the final stage of the bloom post chain. It
// samples two runtime-bound inputs through the bindless set-0 array — the original
// HDR lighting target and the blurred bloom residual — adds the bloom scaled by the
// authored Intensity, and writes the sum to a single SV_Target0 (the PostProcess
// output contract). The result is the HDR target the tonemap stage reads, so bloom
// is composited in linear HDR before tone mapping.

struct MaterialParams
{
    uint  Hdr;          // bindless sampled-image index (runtime-bound each frame)
    uint  HdrSampler;   // bindless sampler index (runtime-bound each frame)
    uint  Bloom;        // bindless sampled-image index (runtime-bound each frame)
    uint  BloomSampler; // bindless sampler index (runtime-bound each frame)
    float Intensity;    // authored param: the bloom mix
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

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(g_PC.MaterialIndex);

    float3 hdr = g_Textures[NonUniformResourceIndex(p.Hdr)].Sample(
        g_Samplers[NonUniformResourceIndex(p.HdrSampler)], input.v_UV).rgb;
    float3 bloom = g_Textures[NonUniformResourceIndex(p.Bloom)].Sample(
        g_Samplers[NonUniformResourceIndex(p.BloomSampler)], input.v_UV).rgb;

    return float4(hdr + bloom * p.Intensity, 1.0);
}
