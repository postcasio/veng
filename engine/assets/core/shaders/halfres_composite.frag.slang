// Half-resolution translucent composite: upsamples the half-res layer (premultiplied color,
// coverage alpha) to full resolution and hands it to the pipeline's (One, OneMinusSrcAlpha)
// blend. The upsample is depth-aware — each of the four half-res taps is weighted by its
// bilinear footprint AND by how closely its reduced depth matches this full-res pixel's own
// opaque depth — so the layer hugs geometry edges instead of haloing across them; over open
// interior regions the depth weights are uniform and the filter degrades to plain bilinear.
[[vk::binding(0, 0)]] Texture2D g_Textures[];

struct PushConstants
{
    uint LayerTexture;
    uint HalfDepthTexture;
    uint DepthTexture;
    uint Pad0;
    // The full-res and half-res valid sub-rect extents, in pixels.
    float2 FullValid;
    float2 HalfValid;
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
    Texture2D layer = g_Textures[NonUniformResourceIndex(g_PC.LayerTexture)];
    Texture2D halfDepth = g_Textures[NonUniformResourceIndex(g_PC.HalfDepthTexture)];
    Texture2D fullDepth = g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)];

    const int2 pixel = int2(input.sv_position.xy);
    const float ownDepth = fullDepth.Load(int3(pixel, 0)).r;

    // The half-res texel-center position this full-res pixel maps to, and the 2x2 tap window
    // around it, clamped inside the half valid sub-rect.
    const float2 halfPos =
        (input.sv_position.xy) * (g_PC.HalfValid / g_PC.FullValid) - 0.5;
    const float2 baseFloor = floor(halfPos);
    const float2 frac = halfPos - baseFloor;
    const int2 base = int2(baseFloor);
    const int2 maxCoord = int2(g_PC.HalfValid) - 1;

    const float4 bilinear = float4((1.0 - frac.x) * (1.0 - frac.y), frac.x * (1.0 - frac.y),
                                   (1.0 - frac.x) * frac.y, frac.x * frac.y);

    float4 accum = 0.0;
    float totalWeight = 0.0;
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        const int2 offset = int2(i & 1, i >> 1);
        const int2 coord = clamp(base + offset, int2(0, 0), maxCoord);
        const float tapDepth = halfDepth.Load(int3(coord, 0)).r;
        // Reverse-Z device depths: a mismatched tap sits across a geometry edge, and its
        // weight collapses in proportion. The epsilon keeps a perfectly matched 2x2 window
        // at the plain bilinear weights.
        const float depthWeight = 1.0 / (1e-4 + abs(ownDepth - tapDepth));
        const float weight = bilinear[i] * depthWeight;
        accum += layer.Load(int3(coord, 0)) * weight;
        totalWeight += weight;
    }
    if (totalWeight <= 0.0)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    return accum / totalWeight;
}
