// Reads a byte-address storage buffer from the set-0 bindless g_Buffers[] array,
// selected by a push-constant handle index, and outputs the loaded float4 as color.
// The buffer array (binding 3) sits beside the sampler array (binding 1) in the same
// set-0 descriptor set: the load and the paired texture sample together exercise a
// non-dynamic, full-range byte-address buffer coexisting with a sampler in one
// argument buffer (the MoltenVK descriptor-layout question). The handle is uniform
// across the draw, so it needs no NonUniformResourceIndex.
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];
[[vk::binding(3, 0)]] ByteAddressBuffer g_Buffers[];

struct PushConstants
{
    uint BufferIndex;  // g_Buffers[] slot the color is loaded from
    uint TextureIndex; // g_Textures[] slot sampled alongside (coexistence check)
    uint SamplerIndex; // g_Samplers[] slot for the paired sample
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
    // The buffer read carries the result; the texture sample proves a sampler and a
    // byte-address buffer array bind together in set 0 without mistranslation.
    float4 fromBuffer = g_Buffers[g_PC.BufferIndex].Load<float4>(0);
    float4 fromTexture = g_Textures[NonUniformResourceIndex(g_PC.TextureIndex)].Sample(
        g_Samplers[NonUniformResourceIndex(g_PC.SamplerIndex)], input.v_UV);
    // Fold in a zero-weighted texture tap so the sampler binding is genuinely used
    // (and cannot be optimized out) while the buffer value drives the output.
    return fromBuffer + fromTexture * 0.0;
}
