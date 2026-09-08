// CMAA2 apply stage: the second fullscreen pass. It reads the edge map the edge pass wrote and the
// tonemapped LDR scene, follows each silhouette to find its run length and the way it turns at each
// end, and blends the pixel across the edge by the morphological coverage that shape implies —
// leaving every non-edge pixel exactly as it was.
//
// This is the conservative half of the algorithm: only pixels on a detected silhouette are touched,
// and the blend is bounded to at most half the neighbour, so it removes stair-stepping without the
// texture-wide softening a luma-directed blur (FXAA) applies. The run search is symmetric and
// length-limited, so a long straight edge blends least in its middle and most at its corners — the
// morphological anti-aliasing signature.
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint ColorTexture; // the tonemapped LDR input (bindless sampled-image index)
    uint EdgeTexture;  // the edge map from the edge pass (R = right edge, G = bottom edge)
    uint Sampler;      // the shared linear sampler (bindless sampler index)
    float2 RcpFrame;   // 1 / extent, in UV per texel
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

// The longest silhouette run the search follows each way; past it the edge is treated as effectively
// straight (blend tapers to zero), which bounds the cost and keeps a near-axis edge stable.
static const int MaxSearchSteps = 16;

float3 ColorAt(float2 uv)
{
    return g_Textures[NonUniformResourceIndex(g_PC.ColorTexture)]
        .SampleLevel(g_Samplers[NonUniformResourceIndex(g_PC.Sampler)], uv, 0.0)
        .rgb;
}

// The edge map at a texel centre (point-sampled through the shared linear sampler at texel centres,
// so no neighbour bleeds in). R = right edge of that pixel, G = bottom edge.
float2 EdgeAt(float2 uv)
{
    return g_Textures[NonUniformResourceIndex(g_PC.EdgeTexture)]
        .SampleLevel(g_Samplers[NonUniformResourceIndex(g_PC.Sampler)], uv, 0.0)
        .rg;
}

// Walks a horizontal edge line (a chain of bottom-edges) from `uv`, stepping by `dir` texels, and
// returns how many steps the edge continued before it ended or turned. `edgeUv` samples the bottom
// edge (G) of the row the line runs along.
int SearchHorizontal(float2 startUv, float2 rcp, float dir)
{
    int distance = 0;
    float2 uv = startUv;
    [unroll]
    for (int i = 0; i < MaxSearchSteps; ++i)
    {
        uv += float2(dir * rcp.x, 0.0);
        if (EdgeAt(uv).g < 0.5) { break; }
        distance += 1;
    }
    return distance;
}

int SearchVertical(float2 startUv, float2 rcp, float dir)
{
    int distance = 0;
    float2 uv = startUv;
    [unroll]
    for (int i = 0; i < MaxSearchSteps; ++i)
    {
        uv += float2(0.0, dir * rcp.y);
        if (EdgeAt(uv).r < 0.5) { break; }
        distance += 1;
    }
    return distance;
}

// The morphological coverage for a pixel sitting on an edge run: distances dNeg/dPos to the two ends
// and whether the run turns toward this pixel at each end (crossN/crossP). A run that turns toward
// the pixel at the near end contributes a triangle of coverage tapering from 0.5 at the corner to 0
// at the run's midpoint; the two ends are summed and clamped, so a straight run (no turns) blends
// nothing and a corner blends most.
float Coverage(int dNeg, int dPos, bool crossN, bool crossP)
{
    const float length = float(dNeg + dPos) + 1.0;
    float coverage = 0.0;
    if (crossN)
    {
        // Triangle from the negative end: 0.5 at the corner (dNeg 0) falling to 0 at the midpoint.
        coverage += max(0.0, 0.5 - (float(dNeg) + 0.5) / length);
    }
    if (crossP)
    {
        coverage += max(0.0, 0.5 - (float(dPos) + 0.5) / length);
    }
    return saturate(coverage);
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    const float2 uv = input.v_UV;
    const float2 rcp = g_PC.RcpFrame;

    const float3 centerColor = ColorAt(uv);

    // This pixel's four boundary edges, gathered from the edge map. The right/bottom edges are its
    // own; the left/top edges are the right/bottom edges of its left/top neighbours.
    const float2 edgeHere = EdgeAt(uv);
    const float topEdge = EdgeAt(uv + float2(0.0, -rcp.y)).g;
    const float leftEdge = EdgeAt(uv + float2(-rcp.x, 0.0)).r;
    const float bottomEdge = edgeHere.g;
    const float rightEdge = edgeHere.r;

    float3 result = centerColor;
    float totalWeight = 0.0;
    float3 blended = float3(0.0);

    // A horizontal silhouette (top or bottom edge) blends this pixel vertically. Follow the edge run
    // and see whether it turns up or down at its ends (a perpendicular vertical edge at the end
    // texel), which is what makes the anti-aliasing follow the true slope rather than smear.
    if (bottomEdge > 0.5 || topEdge > 0.5)
    {
        // Follow the run along whichever horizontal edge is present at this pixel; prefer the bottom.
        const bool useBottom = bottomEdge > 0.5;
        const float2 lineUv = useBottom ? uv : (uv + float2(0.0, -rcp.y));
        const int dNeg = SearchHorizontal(lineUv, rcp, -1.0);
        const int dPos = SearchHorizontal(lineUv, rcp, 1.0);
        // A turn at an end is a vertical edge just past where the horizontal run stopped.
        const float2 endNeg = lineUv + float2(-float(dNeg + 1) * rcp.x, 0.0);
        const float2 endPos = lineUv + float2(float(dPos + 1) * rcp.x, 0.0);
        const bool crossN = EdgeAt(endNeg).r > 0.5 || EdgeAt(endNeg + float2(-rcp.x, 0.0)).r > 0.5;
        const bool crossP = EdgeAt(endPos).r > 0.5 || EdgeAt(endPos + float2(-rcp.x, 0.0)).r > 0.5;
        const float coverage = Coverage(dNeg, dPos, crossN, crossP);
        // Blend across the present edge: toward the pixel below for a bottom edge, above for a top.
        const float2 acrossUv = useBottom ? (uv + float2(0.0, rcp.y)) : (uv + float2(0.0, -rcp.y));
        blended += coverage * ColorAt(acrossUv);
        totalWeight += coverage;
    }

    // A vertical silhouette (left or right edge) blends horizontally, by the same construction.
    if (rightEdge > 0.5 || leftEdge > 0.5)
    {
        const bool useRight = rightEdge > 0.5;
        const float2 lineUv = useRight ? uv : (uv + float2(-rcp.x, 0.0));
        const int dNeg = SearchVertical(lineUv, rcp, -1.0);
        const int dPos = SearchVertical(lineUv, rcp, 1.0);
        const float2 endNeg = lineUv + float2(0.0, -float(dNeg + 1) * rcp.y);
        const float2 endPos = lineUv + float2(0.0, float(dPos + 1) * rcp.y);
        const bool crossN = EdgeAt(endNeg).g > 0.5 || EdgeAt(endNeg + float2(0.0, -rcp.y)).g > 0.5;
        const bool crossP = EdgeAt(endPos).g > 0.5 || EdgeAt(endPos + float2(0.0, -rcp.y)).g > 0.5;
        const float coverage = Coverage(dNeg, dPos, crossN, crossP);
        const float2 acrossUv = useRight ? (uv + float2(rcp.x, 0.0)) : (uv + float2(-rcp.x, 0.0));
        blended += coverage * ColorAt(acrossUv);
        totalWeight += coverage;
    }

    // Blend the center toward the accumulated across-edge colour by the total coverage (bounded to
    // 0.5 per axis, so at most the neighbour average). No edge → totalWeight 0 → the pixel is intact.
    if (totalWeight > 0.0)
    {
        const float w = min(totalWeight, 1.0);
        result = lerp(centerColor, blended / max(totalWeight, 1e-5), w);
    }

    return float4(result, 1.0);
}
