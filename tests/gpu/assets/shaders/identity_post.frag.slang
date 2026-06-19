// Identity PostProcess fragment fixture: samples its one runtime-bound input
// texture/sampler through the bindless set-0 array and writes it unchanged to the
// single color target (the PostProcess domain's output contract — one float4
// SV_Target0). It proves the PostProcess fullscreen-material path end to end:
// build-from-material, set-0 bind, upstream sample, selector push at offset 0,
// fullscreen draw, output written.
//
// The push block is the per-draw MaterialIndex selector alone, at offset 0 — a
// PostProcess shader has no geometry block, so the selector sits at the start of
// the push range (the PostProcess domain's selector offset).

struct MaterialParams
{
    uint Source;        // runtime-bound sampled-image handle
    uint SourceSampler; // runtime-bound sampler handle
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
    return g_Textures[NonUniformResourceIndex(p.Source)].Sample(
        g_Samplers[NonUniformResourceIndex(p.SourceSampler)], input.v_UV);
}
