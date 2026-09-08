// CMAA2 edge-detection stage: the first of the two fullscreen passes of the conservative
// morphological anti-aliaser. It reads the tonemapped LDR scene and writes a two-channel edge map —
// R = a "right" edge (the boundary between this pixel and the one to its right), G = a "bottom" edge
// (the boundary to the pixel below) — which the apply pass follows to blend silhouettes.
//
// "Conservative" is the local-contrast adaptation: a boundary is an edge only when its luma step
// both clears an absolute floor and dominates the steps around it, so a smooth gradient produces no
// edges and only genuine silhouettes are ever touched. This is what distinguishes it from FXAA's
// single global threshold, which softens gradient regions the eye reads as smooth.
//
// The tonemap output is linear (the swapchain composite does the display encode), so luma is taken
// in a cheap perceptual (sqrt) space, matching the FXAA pass.
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint Texture;    // the tonemapped LDR input (bindless sampled-image index)
    uint Sampler;    // the shared linear sampler (bindless sampler index)
    float2 RcpFrame; // 1 / input extent, in UV per texel
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

// CMAA2's default edge thresholds. EdgeThreshold is the absolute luma-step floor; LocalContrast
// scales how strongly a step must dominate its neighbours to survive as an edge.
static const float EdgeThreshold = 0.07;
static const float LocalContrastAdaptation = 0.5;

float Luma(float3 linearColor)
{
    const float3 gamma = sqrt(max(linearColor, 0.0));
    return dot(gamma, float3(0.299, 0.587, 0.114));
}

float LumaAt(float2 uv)
{
    return Luma(g_Textures[NonUniformResourceIndex(g_PC.Texture)]
                    .SampleLevel(g_Samplers[NonUniformResourceIndex(g_PC.Sampler)], uv, 0.0)
                    .rgb);
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    const float2 uv = input.v_UV;
    const float2 rcp = g_PC.RcpFrame;

    // The 3x2 / 2x3 luma neighbourhood needed to measure both this pixel's steps and the parallel
    // steps one row/column over, which the local-contrast test compares against.
    const float lumaC = LumaAt(uv);
    const float lumaR = LumaAt(uv + float2(rcp.x, 0.0));      // right
    const float lumaB = LumaAt(uv + float2(0.0, rcp.y));      // bottom
    const float lumaL = LumaAt(uv + float2(-rcp.x, 0.0));     // left
    const float lumaT = LumaAt(uv + float2(0.0, -rcp.y));     // top
    const float lumaRB = LumaAt(uv + float2(rcp.x, rcp.y));   // bottom-right

    const float deltaRight = abs(lumaC - lumaR);
    const float deltaBottom = abs(lumaC - lumaB);

    // Local contrast: the largest parallel step in the immediate neighbourhood. The right edge is
    // compared against the other horizontal steps around it (left step, and the horizontal steps of
    // the pixels above/below via the corner), the bottom edge against the vertical steps likewise.
    const float deltaLeft = abs(lumaC - lumaL);
    const float deltaTop = abs(lumaC - lumaT);
    const float deltaRightBottom = abs(lumaR - lumaRB);   // right pixel's bottom step
    const float deltaBottomRight = abs(lumaB - lumaRB);   // bottom pixel's right step

    const float horizontalContrast =
        max(max(deltaRight, deltaLeft), deltaBottomRight) * LocalContrastAdaptation;
    const float verticalContrast =
        max(max(deltaBottom, deltaTop), deltaRightBottom) * LocalContrastAdaptation;

    const float rightEdge =
        (deltaRight >= EdgeThreshold && deltaRight >= horizontalContrast) ? 1.0 : 0.0;
    const float bottomEdge =
        (deltaBottom >= EdgeThreshold && deltaBottom >= verticalContrast) ? 1.0 : 0.0;

    return float4(rightEdge, bottomEdge, 0.0, 1.0);
}
