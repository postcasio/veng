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

// The largest per-channel radiance a single half-res tap may contribute. The layer composites into
// the lit colour ahead of the bloom bright-pass, so one blown-out texel — a grazing-limb sample a
// half-res footprint magnifies across its whole 2x2 — would bloom across the frame, and the
// bounded-average below faithfully passes through any value it is handed. This caps that without
// touching ordinary content, which sits orders of magnitude under it. A first value, tunable.
static const float HalfResLayerMaxRadiance = 64.0;

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
        // Reverse-Z device depths compare RELATIVELY: device depth is ~near/distance, so a
        // fixed relative difference means a fixed relative distance gap at any range — where an
        // absolute epsilon is mis-scaled for everything except one depth regime (a scene whose
        // far content sits at 1e-6 would read every tap as matched against an absolute 1e-4).
        // A matched tap (relDiff ~ 0) outweighs a cross-edge tap (relDiff >= 1) about 10:1;
        // a window with no depth edge degrades to the plain bilinear weights.
        const float relDiff =
            abs(ownDepth - tapDepth) / max(min(ownDepth, tapDepth), 1e-6);
        const float depthWeight = 1.0 / (0.1 + min(relDiff, 10.0));
        // Sanitize the tap before it is weighted in: a non-finite value (a NaN from a degenerate
        // grazing-limb shell evaluation) would propagate through accum/totalWeight as a flickering
        // bright pixel, and a runaway HDR spike would bloom. Zero the non-finite, cap the finite.
        float4 tap = layer.Load(int3(coord, 0));
        if (any(isnan(tap)) || any(isinf(tap)))
        {
            tap = float4(0.0, 0.0, 0.0, 0.0);
        }
        tap.rgb = min(tap.rgb, float3(HalfResLayerMaxRadiance, HalfResLayerMaxRadiance,
                                      HalfResLayerMaxRadiance));
        const float weight = bilinear[i] * depthWeight;
        accum += tap * weight;
        totalWeight += weight;
    }
    if (totalWeight <= 0.0)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    return accum / totalWeight;
}
