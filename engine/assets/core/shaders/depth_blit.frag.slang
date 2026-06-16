// Depth-visualize fragment stage: samples the g-buffer depth target through the
// bindless set-0 array and writes a greyscale view of it. Reversed-Z is not in
// use, so near is 0 and far is 1; the raw [0, 1] device depth crowds almost
// everything near 1 for a perspective projection, so this inverts it (1 - depth)
// so nearer geometry reads brighter — the conventional depth-buffer debug view.
// A debug view of the deferred g-buffer's depth attachment, the only depth
// target read as a texture in the engine.
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint DepthTexture;
    uint Sampler;
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
    float depth = g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)].Sample(
        g_Samplers[NonUniformResourceIndex(g_PC.Sampler)], input.v_UV).r;
    float shade = saturate(1.0 - depth);
    return float4(shade, shade, shade, 1.0);
}
