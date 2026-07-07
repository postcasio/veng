// Scene-color copy fragment stage: samples the lit scene color and the opaque depth and
// writes them, unclamped, into the refraction scene-color and scene-depth intermediates
// ahead of the translucent pass — so a translucent fragment can sample the opaque scene
// behind itself and depth-test its distorted samples against the geometry that produced
// them. Unlike the TAA history copy it maps its UV through the dynamic-resolution sub-rect:
// sources and destinations share one allocation extent, so the fullscreen triangle's [0,1]
// UV over the valid-extent viewport is scaled to the sources' physical sub-rect (clamped a
// half texel inside it).
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint SourceTexture; // the lit scene-color target (bindless index)
    uint DepthTexture;  // the opaque depth target (bindless index)
    uint Sampler;       // shared sampler
    uint Pad0;
    float2 ScaleUV;     // validExtent / allocExtent (this frame's sub-rect mapping)
    float2 MaxUV;       // (validExtent - 0.5) / allocExtent (half-texel inset clamp)
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

struct CopyOutput
{
    float4 Color : SV_Target0; // the lit scene color, unclamped HDR
    float Depth : SV_Target1;  // the opaque depth, raw [0,1]
};

[shader("fragment")]
CopyOutput fsMain(VSOutput input)
{
    const float2 uv = min(input.v_UV * g_PC.ScaleUV, g_PC.MaxUV);
    SamplerState samp = g_Samplers[NonUniformResourceIndex(g_PC.Sampler)];

    CopyOutput o;
    o.Color =
        float4(g_Textures[NonUniformResourceIndex(g_PC.SourceTexture)].Sample(samp, uv).rgb, 1.0);
    o.Depth = g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)].Sample(samp, uv).r;
    return o;
}
