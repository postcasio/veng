// Debug-draw line vertex stage. Expands each two-endpoint segment (consecutive records in
// the per-frame SSBO at set 1 binding 0) into a screen-space-thickened triangle ribbon: six
// vertices selected by the vertex index, offset perpendicular to the segment in pixel space by
// the record's Width so the ribbon is a constant pixel width regardless of camera distance
// (MoltenVK/Metal has no wide-line support, so a 1-pixel hardware line cannot be thickened).
// Passes the linear-RGBA color, the logical screen UV, and the fragment's NDC depth to the
// fragment for the depth-aware occlusion fade — the same VSOutput the LineList stage produced.
#include "view_constants.slang"

// One GPU line segment-endpoint record. vec4 first (std430 packs it at a 16-byte boundary);
// Position + the pixel ribbon width trail. Mirrors the CPU GpuLineVertex the pass uploads.
struct GpuLineVertex
{
    float4 Color;     // linear RGBA
    float3 Position;  // world-space endpoint
    float Width;      // ribbon thickness in screen pixels
};

[[vk::binding(0, 1)]] StructuredBuffer<GpuLineVertex> g_Lines;

struct PushConstants
{
    uint DepthTexture;        // g-buffer depth bindless index
    uint Sampler;             // shared sampler bindless index
    uint ViewConstantsIndex;  // current frame-in-flight's view-constants region
    uint Pad0;
    float OccludedFade;       // color scale applied to an occluded fragment (0..1)
    float ViewportWidth;      // output extent in pixels, for the pixel→NDC offset
    float ViewportHeight;
    uint Pad3;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float4 v_Color : COLOR0;
    float2 v_ScreenUV : TEXCOORD0;  // logical [0,1] screen UV for the depth sample
    float v_NdcDepth : TEXCOORD1;   // this fragment's NDC depth, for the occlusion compare
};

[shader("vertex")]
VSOutput vsMain()
{
    // Read the SPIR-V VertexIndex builtin directly rather than SV_VertexID: the latter subtracts
    // the base-vertex offset and so pulls in the DrawParameters capability, which the device does
    // not enable (the same reason debug_billboard.vert reads it this way).
    uint vertexId = uint(spirv_asm { result:$$int = OpLoad builtin(VertexIndex:int); });

    // A single non-instanced draw of 6*N vertices: vertexId / 6 selects the segment, vertexId % 6
    // the ribbon-quad corner. The two endpoints are records [segment*2] and [segment*2 + 1].
    uint segment = vertexId / 6;
    uint cornerId = vertexId % 6;

    GpuLineVertex va = g_Lines[segment * 2 + 0];
    GpuLineVertex vb = g_Lines[segment * 2 + 1];

    ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);
    float4 clipA = mul(view.CurViewProj, float4(va.Position, 1.0));
    float4 clipB = mul(view.CurViewProj, float4(vb.Position, 1.0));

    // Near-plane clip in clip space: the screen-space expansion below divides by w, so an endpoint
    // behind the camera (w <= 0) must first be pulled to the near plane along the segment (the
    // hardware line rasterizer clipped this for free). A segment fully behind collapses off-screen.
    const float wEps = 1e-4;
    bool aBehind = clipA.w < wEps;
    bool bBehind = clipB.w < wEps;
    if (aBehind && bBehind)
    {
        VSOutput culled;
        culled.sv_position = float4(0.0, 0.0, -2.0, 1.0);  // z outside [0,1] → clipped away
        culled.v_Color = float4(0.0, 0.0, 0.0, 0.0);
        culled.v_ScreenUV = float2(0.0, 0.0);
        culled.v_NdcDepth = 0.0;
        return culled;
    }
    if (aBehind)
    {
        clipA = lerp(clipA, clipB, (wEps - clipA.w) / (clipB.w - clipA.w));
    }
    if (bBehind)
    {
        clipB = lerp(clipB, clipA, (wEps - clipB.w) / (clipA.w - clipB.w));
    }

    // Segment direction in pixel space (NDC scaled by the half-extent), so the perpendicular
    // offset is an exact pixel width. A degenerate (zero-length) segment collapses harmlessly.
    float2 halfExtent = float2(g_PC.ViewportWidth, g_PC.ViewportHeight) * 0.5;
    float2 pixA = (clipA.xy / clipA.w) * halfExtent;
    float2 pixB = (clipB.xy / clipB.w) * halfExtent;
    float2 delta = pixB - pixA;
    float2 dir = (dot(delta, delta) > 1e-12) ? normalize(delta) : float2(1.0, 0.0);
    float2 normal = float2(-dir.y, dir.x);

    // Ribbon-quad corners (two triangles): bit 0 picks the endpoint (A/B), the sign picks the side.
    static const uint useB[6] = { 0, 1, 0, 1, 1, 0 };
    static const float side[6] = { -1.0, -1.0, 1.0, -1.0, 1.0, 1.0 };

    bool pickB = useB[cornerId] != 0;
    float4 clipE = pickB ? clipB : clipA;
    GpuLineVertex ve = pickB ? vb : va;

    // Offset the chosen endpoint by half the pixel width along the screen-space normal, converted
    // back to an NDC offset (pixels / half-extent) and re-homogenized by the endpoint's clip w.
    float2 offsetPix = normal * (ve.Width * 0.5) * side[cornerId];
    float2 ndcE = clipE.xy / clipE.w;
    float2 ndcFinal = ndcE + offsetPix / halfExtent;

    VSOutput output;
    output.sv_position = float4(ndcFinal * clipE.w, clipE.z, clipE.w);
    output.v_Color = ve.Color;
    // Logical screen UV from the offset NDC ([-1,1] → [0,1]). Vulkan clip-space Y points down,
    // so UV.y maps directly without a flip (matching the g-buffer sample sites).
    output.v_ScreenUV = ndcFinal * 0.5 + 0.5;
    output.v_NdcDepth = clipE.z / clipE.w;
    return output;
}
