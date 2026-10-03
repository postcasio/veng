// Composes three per-frame inputs into separate channels so a frame's output names exactly what
// it read: red from the texture registered this frame, green from one registered two frames
// earlier, blue from this frame's slice of a host-written ring. Any slot rewritten, freed, or
// slice overwritten while a submitted frame still reads it shows up as a wrong channel.
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];
[[vk::binding(3, 0)]] ByteAddressBuffer g_Buffers[];

struct PushConstants
{
    uint CurrentTexture; // g_Textures[] slot registered this frame
    uint EarlierTexture; // g_Textures[] slot registered two frames earlier
    uint SamplerIndex;   // g_Samplers[] slot both are sampled through
    uint RingBuffer;     // g_Buffers[] slot of the host-written ring
    uint RingOffset;     // byte offset of this frame's slice in the ring
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
    SamplerState samplerState = g_Samplers[NonUniformResourceIndex(g_PC.SamplerIndex)];
    float current = g_Textures[NonUniformResourceIndex(g_PC.CurrentTexture)].Sample(
        samplerState, input.v_UV).r;
    float earlier = g_Textures[NonUniformResourceIndex(g_PC.EarlierTexture)].Sample(
        samplerState, input.v_UV).r;
    float ring = g_Buffers[g_PC.RingBuffer].Load<float4>(g_PC.RingOffset).x;
    return float4(current, earlier, ring, 1.0);
}
