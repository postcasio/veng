// Spatial promotion: reads the dynamic-resolution sub-rect the scene rasterized into and writes
// the full allocation, so every pass from here on runs at the allocation rather than at the
// sub-rect. fsMain promotes one target — one pipeline per destination format, the HDR scene colour
// or the single-channel bloom mask beside it (a single-channel attachment consumes only the red
// channel) — and fsPair promotes both in one pass when both cross the boundary, each through its
// own mapping and each exactly as fsMain would.
// It occupies the slot the temporal resolve occupies when one is active — that resolve
// reconstructs the allocation as a side effect of reprojecting history, this does the same job for
// a frame with no history to reproject.
//
// One bilinear tap through the sub-rect mapping, which is exactly what the terminal tonemap did
// when it carried this upscale: the same filter on the same linear HDR values in the same order,
// so promoting the upscale to here leaves a frame with nothing between the two unchanged.
// Unclamped, like taa_history_copy and unlike hdr_blit — the tail after it is still linear HDR.
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint SourceTexture;  // the sub-rect scene color (bindless index)
    uint Sampler;        // shared linear clamp-to-edge sampler
    uint MaskTexture;    // fsPair: the sub-rect bloom mask (bindless index)
    uint Pad0;
    float2 ScaleUV;      // validExtent / allocExtent
    float2 MaxUV;        // (validExtent - 0.5) / allocExtent, the bilinear-tap clamp
    float2 MaskScaleUV;  // fsPair: the mask's own mapping, as ScaleUV
    float2 MaskMaxUV;    // fsPair: the mask's own clamp, as MaxUV
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

// One bilinear tap of `source` through its sub-rect mapping.
float4 Promote(uint source, float2 uv, float2 scaleUV, float2 maxUV)
{
    const float3 texel = g_Textures[NonUniformResourceIndex(source)].Sample(
        g_Samplers[NonUniformResourceIndex(g_PC.Sampler)], min(uv * scaleUV, maxUV)).rgb;
    return float4(texel, 1.0);
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target0
{
    return Promote(g_PC.SourceTexture, input.v_UV, g_PC.ScaleUV, g_PC.MaxUV);
}

struct PairOutput
{
    float4 Color : SV_Target0; // the promoted scene colour
    float4 Mask : SV_Target1;  // the promoted bloom mask (its attachment keeps the red channel)
};

[shader("fragment")]
PairOutput fsPair(VSOutput input)
{
    PairOutput output;
    output.Color = Promote(g_PC.SourceTexture, input.v_UV, g_PC.ScaleUV, g_PC.MaxUV);
    output.Mask = Promote(g_PC.MaskTexture, input.v_UV, g_PC.MaskScaleUV, g_PC.MaskMaxUV);
    return output;
}
