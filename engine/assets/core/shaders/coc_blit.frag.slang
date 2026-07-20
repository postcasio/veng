// Circle-of-confusion visualize fragment stage: samples the depth-of-field chain's signed-radius
// buffer (R = signed radius in half-resolution pixels, G = view-space depth) through the bindless
// set-0 array and writes the focus-plane readout: the near field (a negative radius, in front of
// the focus plane) ramps red, the far field (a positive radius) ramps blue, and an in-focus texel
// is black.
//
// The ramp is normalized against the frame's own MaxCoc budget, so brightness reads as the
// fraction of the configured budget a texel uses and a shallow budget still fills the ramp.
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint CocTexture;
    uint Sampler;
    float MaxCoc;
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
    float coc = g_Textures[NonUniformResourceIndex(g_PC.CocTexture)].Sample(
        g_Samplers[NonUniformResourceIndex(g_PC.Sampler)], input.v_UV).r;
    // A non-positive authored budget clamps to zero, so the divisor is floored to keep the whole
    // view finite rather than inf/NaN.
    float budget = max(g_PC.MaxCoc, 1e-4);
    float near = saturate(max(-coc, 0.0) / budget);
    float far = saturate(max(coc, 0.0) / budget);
    return float4(near, 0.0, far, 1.0);
}
