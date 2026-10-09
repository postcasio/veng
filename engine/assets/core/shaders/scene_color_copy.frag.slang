// The pre-translucent grab's copy fragment stages: fsMain samples the lit scene color into the
// refraction scene-color intermediate, and fsDepth samples the opaque depth into the scene-depth
// intermediate, each unclamped and each in its own single-attachment pass ahead of the translucent
// pass — so a translucent fragment can sample the opaque scene behind itself, or only its depth,
// and the renderer records just the copy a frame's materials read. Unlike the TAA history copy
// these map their UV through the dynamic-resolution sub-rect: source and destination share one
// allocation extent, so the fullscreen triangle's [0,1] UV over the valid-extent viewport is scaled
// to the source's physical sub-rect (clamped a half texel inside it).
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint SourceTexture; // the lit scene color (fsMain) or the opaque depth (fsDepth), bindless index
    uint Sampler;       // shared sampler
    uint Pad0;
    uint Pad1;
    float2 ScaleUV;     // validExtent / allocExtent (this frame's sub-rect mapping)
    float2 MaxUV;       // (validExtent - 0.5) / allocExtent (half-texel inset clamp)
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

float4 SampleSource(VSOutput input)
{
    const float2 uv = min(input.v_UV * g_PC.ScaleUV, g_PC.MaxUV);
    SamplerState samp = g_Samplers[NonUniformResourceIndex(g_PC.Sampler)];
    return g_Textures[NonUniformResourceIndex(g_PC.SourceTexture)].Sample(samp, uv);
}

// The lit scene color, unclamped HDR.
[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target0
{
    return float4(SampleSource(input).rgb, 1.0);
}

// The opaque depth, raw [0,1].
[shader("fragment")]
float fsDepth(VSOutput input) : SV_Target0
{
    return SampleSource(input).r;
}
