// Scene-capture cube-face copy fragment stage: samples one captured face's HDR render and writes
// it into that face's layer of the radiance cube, mirroring horizontally.
//
// A face is rendered by a right-handed look-at camera, but a cube is sampled left-handed — a
// texel's sampling direction flips the horizontal face axis relative to the camera image — so
// writing the face straight in would store a per-face mirror of the environment, and the IBL
// convolution reading the cube would light from a reflected world. Flipping the sampled U converts
// the camera image into the cube-sampling convention (ibl_equirect_to_cube's FaceDirection), so the
// cube samples correctly by world direction. Distinct from taa_history_copy, which copies with no
// flip.
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint SourceTexture; // the captured face HDR (bindless index)
    uint Sampler;       // shared clamp sampler
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
    const float2 uv = float2(1.0 - input.v_UV.x, input.v_UV.y);
    const float3 color = g_Textures[NonUniformResourceIndex(g_PC.SourceTexture)].Sample(
        g_Samplers[NonUniformResourceIndex(g_PC.Sampler)], uv).rgb;
    return float4(color, 1.0);
}
