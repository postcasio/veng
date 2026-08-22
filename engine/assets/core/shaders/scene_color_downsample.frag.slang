// One halving level of the refraction scene-color grab's mip chain: samples the level above and
// writes the level below, so a translucent material can read the scene behind it blurred (see
// Veng/translucent.slang's SampleSceneColorBlurred).
//
// **Four bilinear taps, not one.** A single tap at the destination texel's centre is the plain
// 2x2 box a hardware mip would be, and chained eight deep it keeps a visible square structure and
// crawls when the scene moves under it. Four taps at the parent's texel corners cover a 4x4
// neighbourhood with a tent-ish weighting for four fetches, which is what makes the coarse levels
// read as a blur rather than as a mosaic — and this content is usually a star field, where a
// single point must spread and must not repeat.
//
// Like the copy that fills level 0, this maps its UV through the dynamic-resolution sub-rect: every
// level renders only the sub-rect its parent occupied, halved, and clamps its reads inside the
// parent's valid region, so the cleared area outside it never bleeds inward.
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint SourceTexture; // the parent level (bindless index)
    uint Sampler;       // a chain sampler: clamp-to-edge, linear, no LOD clamp
    uint Pad0;
    uint Pad1;
    float2 ScaleUV;     // parentValid / parentAlloc
    float2 MaxUV;       // (parentValid - 0.5) / parentAlloc
    float2 TexelSize;   // 1 / parentAlloc, the offset one parent texel subtends
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

[shader("fragment")]
float4 fsMain(VSOutput input)
{
    const SamplerState samp = g_Samplers[NonUniformResourceIndex(g_PC.Sampler)];
    const Texture2D source = g_Textures[NonUniformResourceIndex(g_PC.SourceTexture)];
    const float2 centre = input.v_UV * g_PC.ScaleUV;
    const float2 step = g_PC.TexelSize;

    float3 sum = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int y = -1; y <= 1; y += 2)
    {
        [unroll]
        for (int x = -1; x <= 1; x += 2)
        {
            const float2 uv = min(centre + float2(x, y) * step, g_PC.MaxUV);
            sum += source.SampleLevel(samp, uv, 0.0).rgb;
        }
    }
    return float4(sum * 0.25, 1.0);
}
