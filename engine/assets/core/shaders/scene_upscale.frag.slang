// Spatial scene-color upscale: reads the dynamic-resolution sub-rect the scene rasterized into
// and writes the full allocation, so every pass from here on runs at the allocation rather than
// at the sub-rect. It occupies the slot the temporal resolve occupies when one is active — that
// resolve reconstructs the allocation as a side effect of reprojecting history, this does the
// same job for a frame with no history to reproject.
//
// One bilinear tap through the sub-rect mapping, which is exactly what the terminal tonemap did
// when it carried this upscale: the same filter on the same linear HDR values in the same order,
// so promoting the upscale to here leaves a frame with nothing between the two unchanged.
// Unclamped, like taa_history_copy and unlike hdr_blit — the tail after it is still linear HDR.
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint SourceTexture; // the sub-rect scene color (bindless index)
    uint Sampler;       // shared linear clamp-to-edge sampler
    uint Pad0;
    uint Pad1;
    float2 ScaleUV;     // validExtent / allocExtent
    float2 MaxUV;       // (validExtent - 0.5) / allocExtent, the bilinear-tap clamp
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
    const float2 uv = min(input.v_UV * g_PC.ScaleUV, g_PC.MaxUV);
    const float3 scene = g_Textures[NonUniformResourceIndex(g_PC.SourceTexture)].Sample(
        g_Samplers[NonUniformResourceIndex(g_PC.Sampler)], uv).rgb;
    return float4(scene, 1.0);
}
