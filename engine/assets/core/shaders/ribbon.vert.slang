// Ribbon vertex stage. Expands each record (the per-frame SSBO at set 3 binding 0) into a quad of
// six vertices selected by the vertex index, each placed in view space from its eye-relative
// position (rotated by the View matrix's rotation alone, so no large world coordinate meets the view
// translation). A segment record's ends are pushed out along the direction perpendicular both to
// the ribbon and to the eye ray, so the quad turns about its own axis to face the camera; a dot
// record spans a square parallel to the image plane, so its disc projects to a circle. An end
// narrower than MinPixelWidth widens to it at proportionally lower opacity. vsMain projects through
// the jittered Proj so the scene placement resolves under TAA with the scene; vsMainPostResolve
// projects through the pushed unjittered projection and floors the width in post-resolve pixels.
#include "view_constants.slang"
#include "ribbon_common.slang"

// One record. Mirrors the CPU GpuRibbonSegment the pass uploads (std430, 96 bytes).
struct GpuRibbonSegment
{
    float4 Start;        // xyz eye-relative start, w world width (a dot's diameter)
    float4 End;          // xyz eye-relative end, w world width
    float4 StartColor;   // rgb linear HDR, a opacity
    float4 EndColor;     // rgb linear HDR, a opacity
    float4 StartTangent; // xyz unit direction at the start; w 1 marks a dot record
    float4 EndTangent;   // xyz unit direction at the end
};

[[vk::binding(0, 3)]] StructuredBuffer<GpuRibbonSegment> g_Segments;

// The narrowest a ribbon draws, in pixels: thinner and it would alias to a broken, shimmering line.
static const float MinPixelWidth = 1.5;

// Rotates an eye-relative vector into view space.
float3 ToView(ViewConstants view, float3 v)
{
    return float3(dot(view.View[0].xyz, v), dot(view.View[1].xyz, v), dot(view.View[2].xyz, v));
}

// Places one corner of a record's quad through proj, flooring its width at MinPixelWidth pixels of
// a viewport targetHeight pixels tall.
VSOutput ExpandRecord(uint vertexId, ViewConstants view, float4x4 proj, float targetHeight)
{
    GpuRibbonSegment segment = g_Segments[vertexId / 6];

    // Two-triangle quad corners: x runs start (0) to end (1), y runs across the width.
    static const float2 corners[6] = {
        float2(0.0, 0.0), float2(1.0, 0.0), float2(0.0, 1.0),
        float2(1.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0),
    };
    float2 corner = corners[vertexId % 6];
    bool atEnd = corner.x > 0.5;

    float4 end = atEnd ? segment.End : segment.Start;
    float4 color = atEnd ? segment.EndColor : segment.StartColor;
    float3 center = ToView(view, end.xyz);

    // World units per pixel at this end's depth, from the projection's vertical scale.
    float4 clipCenter = mul(proj, float4(center, 1.0));
    float worldPerPixel = 2.0 * abs(clipCenter.w) / (abs(proj[1][1]) * targetHeight);
    float halfWidth = max(0.5 * end.w, 0.5 * MinPixelWidth * worldPerPixel);
    float coverage = halfWidth > 0.0 ? (0.5 * end.w) / halfWidth : 0.0;

    float2 profile;
    float3 position;
    if (segment.StartTangent.w > 0.5)
    {
        // Both corners of an end coincide on a dot, so the quad's x and y both span the disc.
        profile = corner * 2.0 - 1.0;
        position = center + float3(profile * halfWidth, 0.0);
    }
    else
    {
        float3 tangent = ToView(view, atEnd ? segment.EndTangent.xyz : segment.StartTangent.xyz);
        // Perpendicular to the ribbon and to the eye ray; looking straight down the ribbon leaves
        // no such direction, and any one perpendicular to the ribbon serves.
        float3 side = cross(tangent, center);
        if (dot(side, side) < 1e-12)
        {
            side = cross(tangent,
                         abs(tangent.y) < 0.9 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0));
        }
        side = normalize(side);
        float across = corner.y * 2.0 - 1.0;
        profile = float2(across, 0.0);
        position = center + side * (across * halfWidth);
    }

    VSOutput output;
    output.sv_position = mul(proj, float4(position, 1.0));
    output.v_Color = float4(color.rgb, color.a * coverage);
    output.v_Profile = profile;
    output.v_ViewDepth = -position.z;
    return output;
}

// The VertexIndex builtin read directly (it includes the draw's firstVertex, which selects the
// alpha or additive run); SV_VertexID would pull in the DrawParameters capability.
uint LoadVertexIndex()
{
    return uint(spirv_asm { result:$$int = OpLoad builtin(VertexIndex:int); });
}

[shader("vertex")]
VSOutput vsMain()
{
    ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);
    return ExpandRecord(LoadVertexIndex(), view, view.Proj, view.ExtentParams.y);
}

[shader("vertex")]
VSOutput vsMainPostResolve()
{
    ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);
    return ExpandRecord(LoadVertexIndex(), view, g_PC.Proj, g_PC.TargetExtent.y);
}
