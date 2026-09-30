// Ribbon vertex stage. Expands each segment record (the per-frame SSBO at set 3 binding 0) into a
// camera-facing quad: six vertices selected by the vertex index, each end placed in view space
// from its eye-relative position (rotated by the View matrix's rotation alone, so no large world
// coordinate meets the view translation), then pushed out along the direction perpendicular both
// to the ribbon and to the eye ray, so the quad turns about its own axis to face the camera. A
// segment end narrower than MinPixelWidth widens to it at proportionally lower opacity. Projected
// through the jittered Proj so ribbons resolve under TAA with the scene.
#include "view_constants.slang"

// One segment record. Mirrors the CPU GpuRibbonSegment the pass uploads (std430, 96 bytes).
struct GpuRibbonSegment
{
    float4 Start;        // xyz eye-relative start, w world width
    float4 End;          // xyz eye-relative end, w world width
    float4 StartColor;   // rgb linear HDR, a opacity
    float4 EndColor;     // rgb linear HDR, a opacity
    float4 StartTangent; // xyz unit direction at the start
    float4 EndTangent;   // xyz unit direction at the end
};

[[vk::binding(0, 3)]] StructuredBuffer<GpuRibbonSegment> g_Segments;

struct PushConstants
{
    uint ViewConstantsIndex;
    uint Pad0;
    uint Pad1;
    uint Pad2;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float4 v_Color : COLOR0;
    float v_Across : TEXCOORD0;
};

// The narrowest a ribbon draws, in pixels: thinner and it would alias to a broken, shimmering line.
static const float MinPixelWidth = 1.5;

// Rotates an eye-relative vector into view space.
float3 ToView(ViewConstants view, float3 v)
{
    return float3(dot(view.View[0].xyz, v), dot(view.View[1].xyz, v), dot(view.View[2].xyz, v));
}

[shader("vertex")]
VSOutput vsMain()
{
    // The VertexIndex builtin read directly (it includes the draw's firstVertex, which selects the
    // alpha or additive run); SV_VertexID would pull in the DrawParameters capability.
    uint vertexId = uint(spirv_asm { result:$$int = OpLoad builtin(VertexIndex:int); });
    GpuRibbonSegment segment = g_Segments[vertexId / 6];
    ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);

    // Two-triangle quad corners: x runs start (0) to end (1), y runs across the width.
    static const float2 corners[6] = {
        float2(0.0, 0.0), float2(1.0, 0.0), float2(0.0, 1.0),
        float2(1.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0),
    };
    float2 corner = corners[vertexId % 6];
    bool atEnd = corner.x > 0.5;

    float4 end = atEnd ? segment.End : segment.Start;
    float4 color = atEnd ? segment.EndColor : segment.StartColor;
    float3 tangent = ToView(view, atEnd ? segment.EndTangent.xyz : segment.StartTangent.xyz);
    float3 center = ToView(view, end.xyz);

    // Perpendicular to the ribbon and to the eye ray; looking straight down the ribbon leaves no
    // such direction, and any one perpendicular to the ribbon serves.
    float3 side = cross(tangent, center);
    if (dot(side, side) < 1e-12)
    {
        side = cross(tangent, abs(tangent.y) < 0.9 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0));
    }
    side = normalize(side);

    // World units per pixel at this end's depth, from the projection's vertical scale.
    float4 clipCenter = mul(view.Proj, float4(center, 1.0));
    float worldPerPixel = 2.0 * abs(clipCenter.w) / (abs(view.Proj[1][1]) * view.ExtentParams.y);
    float halfWidth = max(0.5 * end.w, 0.5 * MinPixelWidth * worldPerPixel);
    float coverage = halfWidth > 0.0 ? (0.5 * end.w) / halfWidth : 0.0;

    float across = corner.y * 2.0 - 1.0;
    float3 position = center + side * (across * halfWidth);

    VSOutput output;
    output.sv_position = mul(view.Proj, float4(position, 1.0));
    output.v_Color = float4(color.rgb, color.a * coverage);
    output.v_Across = across;
    return output;
}
