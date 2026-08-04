// Depth-visualize fragment stage: samples the g-buffer depth target through the
// bindless set-0 array and writes a greyscale view of it. Reverse-Z is in use, so
// near is 1 and far is 0; the raw [0, 1] device depth already reads brighter for
// nearer geometry, so this writes it straight through — the conventional
// nearer-is-brighter depth-buffer debug view.
// A debug view of the deferred g-buffer's depth attachment, one of the two depth
// targets read as textures in the engine (the other is the directional shadow map).
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
    float shade = saturate(depth);
    return float4(shade, shade, shade, 1.0);
}
