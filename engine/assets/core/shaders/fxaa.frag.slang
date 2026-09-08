// FXAA fragment stage: a single post-tonemap luma-directed edge blur (Lottes' FXAA 3.11 quality
// path, condensed). It samples the tonemapped LDR scene through the bindless set-0 array, finds the
// local edge from a 3x3 luma neighbourhood, and blends along it — a cheap spatial anti-aliaser with
// no history and no motion vectors, the fallback the temporal path (TAA) trades softness for.
//
// The tonemap output is linear (the swapchain composite does the display encode), so the edge
// detector needs a perceptual luma to key on: sqrt(linear) approximates the sRGB transfer closely
// enough over [0,1] to place edges where the eye sees them, and is far cheaper than a full encode.
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint Texture;      // the tonemapped LDR input (bindless sampled-image index)
    uint Sampler;      // the shared linear sampler (bindless sampler index)
    float2 RcpFrame;   // 1 / output extent, in UV per texel
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

// FXAA console/PC quality tunables (Lottes' recommended defaults).
static const float EdgeThreshold = 0.166;      // minimum local contrast to treat as an edge
static const float EdgeThresholdMin = 0.0833;  // darkness floor below which edges are ignored
static const float SubpixelQuality = 0.75;     // how much sub-pixel (thin-feature) aliasing to remove
static const int SearchSteps = 12;             // end-of-edge search iterations along the edge

// Perceptual luma of a linear colour: sqrt is a cheap, monotone stand-in for the sRGB transfer that
// keeps the detector's contrast comparisons meaningful on the linear tonemap output.
float Luma(float3 linearColor)
{
    const float3 gamma = sqrt(max(linearColor, 0.0));
    return dot(gamma, float3(0.299, 0.587, 0.114));
}

float3 SampleRgb(float2 uv)
{
    return g_Textures[NonUniformResourceIndex(g_PC.Texture)]
        .SampleLevel(g_Samplers[NonUniformResourceIndex(g_PC.Sampler)], uv, 0.0)
        .rgb;
}

float LumaAt(float2 uv)
{
    return Luma(SampleRgb(uv));
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    const float2 uv = input.v_UV;
    const float2 rcp = g_PC.RcpFrame;

    const float3 centerColor = SampleRgb(uv);
    const float lumaCenter = Luma(centerColor);

    // The four axis neighbours plus the range they span; below the threshold the pixel is flat and
    // the center colour passes through unblurred.
    const float lumaN = LumaAt(uv + float2(0.0, -rcp.y));
    const float lumaS = LumaAt(uv + float2(0.0, rcp.y));
    const float lumaE = LumaAt(uv + float2(rcp.x, 0.0));
    const float lumaW = LumaAt(uv + float2(-rcp.x, 0.0));

    const float lumaMin = min(lumaCenter, min(min(lumaN, lumaS), min(lumaE, lumaW)));
    const float lumaMax = max(lumaCenter, max(max(lumaN, lumaS), max(lumaE, lumaW)));
    const float lumaRange = lumaMax - lumaMin;

    if (lumaRange < max(EdgeThresholdMin, lumaMax * EdgeThreshold))
    {
        return float4(centerColor, 1.0);
    }

    // The four diagonal neighbours complete the 3x3 gradient estimate.
    const float lumaNW = LumaAt(uv + float2(-rcp.x, -rcp.y));
    const float lumaNE = LumaAt(uv + float2(rcp.x, -rcp.y));
    const float lumaSW = LumaAt(uv + float2(-rcp.x, rcp.y));
    const float lumaSE = LumaAt(uv + float2(rcp.x, rcp.y));

    const float lumaNS = lumaN + lumaS;
    const float lumaWE = lumaW + lumaE;
    const float lumaNWNE = lumaNW + lumaNE;
    const float lumaSWSE = lumaSW + lumaSE;
    const float lumaNWSW = lumaNW + lumaSW; // west column corners
    const float lumaNESE = lumaNE + lumaSE; // east column corners

    // Sobel-like edge orientation: horizontal edge (blur vertically) vs vertical edge.
    const float edgeHorizontal = abs(-2.0 * lumaW + lumaNWSW) +
                                 abs(-2.0 * lumaCenter + lumaNS) * 2.0 +
                                 abs(-2.0 * lumaE + lumaNESE);
    const float edgeVertical = abs(-2.0 * lumaN + lumaNWNE) +
                               abs(-2.0 * lumaCenter + lumaWE) * 2.0 +
                               abs(-2.0 * lumaS + lumaSWSE);
    const bool isHorizontal = edgeHorizontal >= edgeVertical;

    // Step one texel across the edge, toward the steeper of the two sides.
    float luma1 = isHorizontal ? lumaS : lumaE;
    float luma2 = isHorizontal ? lumaN : lumaW;
    const float gradient1 = abs(luma1 - lumaCenter);
    const float gradient2 = abs(luma2 - lumaCenter);
    const bool is1Steepest = gradient1 >= gradient2;
    const float gradientScaled = 0.25 * max(gradient1, gradient2);

    const float stepLength = isHorizontal ? rcp.y : rcp.x;
    float lumaLocalAverage;
    if (is1Steepest)
    {
        lumaLocalAverage = 0.5 * (luma1 + lumaCenter);
    }
    else
    {
        lumaLocalAverage = 0.5 * (luma2 + lumaCenter);
    }
    const float stepSign = is1Steepest ? -stepLength : stepLength;

    float2 currentUv = uv;
    if (isHorizontal)
    {
        currentUv.y += stepSign * 0.5;
    }
    else
    {
        currentUv.x += stepSign * 0.5;
    }

    // Walk both ways along the edge until the luma diverges past the gradient, giving the edge's
    // endpoints; the pixel's position between them sets the blend offset.
    const float2 offset = isHorizontal ? float2(rcp.x, 0.0) : float2(0.0, rcp.y);
    float2 uv1 = currentUv - offset;
    float2 uv2 = currentUv + offset;

    float lumaEnd1 = LumaAt(uv1) - lumaLocalAverage;
    float lumaEnd2 = LumaAt(uv2) - lumaLocalAverage;
    bool reached1 = abs(lumaEnd1) >= gradientScaled;
    bool reached2 = abs(lumaEnd2) >= gradientScaled;
    bool reachedBoth = reached1 && reached2;

    if (!reached1) { uv1 -= offset; }
    if (!reached2) { uv2 += offset; }

    [unroll]
    for (int i = 0; i < SearchSteps; ++i)
    {
        if (reachedBoth) { break; }
        if (!reached1)
        {
            lumaEnd1 = LumaAt(uv1) - lumaLocalAverage;
            reached1 = abs(lumaEnd1) >= gradientScaled;
        }
        if (!reached2)
        {
            lumaEnd2 = LumaAt(uv2) - lumaLocalAverage;
            reached2 = abs(lumaEnd2) >= gradientScaled;
        }
        if (!reached1) { uv1 -= offset; }
        if (!reached2) { uv2 += offset; }
        reachedBoth = reached1 && reached2;
    }

    const float distance1 = isHorizontal ? (uv.x - uv1.x) : (uv.y - uv1.y);
    const float distance2 = isHorizontal ? (uv2.x - uv.x) : (uv2.y - uv.y);
    const bool isDirection1 = distance1 < distance2;
    const float distanceFinal = min(distance1, distance2);
    const float edgeThickness = distance1 + distance2;
    const float pixelOffset = -distanceFinal / max(edgeThickness, 1e-5) + 0.5;

    // Only blend if the pixel sits on the darker side of the edge (the endpoint whose luma variation
    // matches the center), the FXAA correctness guard against smearing the wrong side.
    const bool isLumaCenterSmaller = lumaCenter < lumaLocalAverage;
    const float endLuma = isDirection1 ? lumaEnd1 : lumaEnd2;
    const bool correctVariation = (endLuma < 0.0) != isLumaCenterSmaller;
    float finalOffset = correctVariation ? pixelOffset : 0.0;

    // Sub-pixel aliasing: a 3x3 luma average measures thin-feature contrast the edge search misses;
    // blend toward it, capped, and take the stronger of the two offsets.
    const float lumaAverage =
        (1.0 / 12.0) * (2.0 * (lumaNS + lumaWE) + lumaNWSW + lumaNESE);
    const float subPixelOffset1 =
        saturate(abs(lumaAverage - lumaCenter) / max(lumaRange, 1e-5));
    const float subPixelOffset2 = (-2.0 * subPixelOffset1 + 3.0) * subPixelOffset1 * subPixelOffset1;
    const float subPixelOffsetFinal = subPixelOffset2 * subPixelOffset2 * SubpixelQuality;
    finalOffset = max(finalOffset, subPixelOffsetFinal);

    float2 finalUv = uv;
    if (isHorizontal)
    {
        finalUv.y += finalOffset * stepSign;
    }
    else
    {
        finalUv.x += finalOffset * stepSign;
    }

    return float4(SampleRgb(finalUv), 1.0);
}
