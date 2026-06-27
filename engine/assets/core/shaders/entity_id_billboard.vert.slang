// Entity-id picking billboard vertex stage. The id-write sibling of debug_billboard.vert: it
// expands each pickable billboard record into a camera-facing proxy quad and forwards the
// owning entity's pick id to the fragment. The proxy footprint is a fixed minimum screen-space
// size (not the icon art): the quad's projected half-extent is clamped up to MinPixelRadius so a
// small or thin icon stays a comfortable, predictable hit target. The fragment masks it to a
// centered disc and writes the pick id; hardware depth-tests against the scene's picking depth
// discard an occluded fragment, so an icon behind geometry is not picked.
#include "view_constants.slang"

// One GPU billboard pick record. vec4 first (std430), then the pick id + min radius + padding.
struct GpuBillboardPick
{
    float4 PositionSize;  // xyz world center, w edge length in world units
    uint PickId;          // owning entity's pick id (packed slot index + 1)
    float MinPixelRadius; // proxy footprint minimum half-extent in pixels (per record)
    uint Pad0;
    uint Pad1;
};

[[vk::binding(0, 1)]] StructuredBuffer<GpuBillboardPick> g_Billboards;

struct PushConstants
{
    uint ViewConstantsIndex;
    float RenderWidth;     // valid sub-rect width in texels (for the NDC↔pixel conversion)
    float RenderHeight;    // valid sub-rect height in texels
    uint Pad0;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_Quad : TEXCOORD0;  // corner in [-1, 1] for the disc mask
    nointerpolation uint v_PickId : TEXCOORD1;
};

[shader("vertex")]
VSOutput vsMain()
{
    // Read VertexIndex directly (not SV_VertexID) to avoid the DrawParameters capability, the same
    // way debug_billboard.vert and fullscreen.vert do — a single non-instanced draw of 6*N vertices.
    uint vertexId = uint(spirv_asm { result:$$int = OpLoad builtin(VertexIndex:int); });
    uint instanceId = vertexId / 6;
    uint cornerId = vertexId % 6;

    GpuBillboardPick bb = g_Billboards[instanceId];
    ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);

    static const float2 corners[6] = {
        float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0),
        float2(1.0, -1.0),  float2(1.0, 1.0),  float2(-1.0, 1.0),
    };
    float2 corner = corners[cornerId];

    // Project the center, then offset corners in NDC. The proxy's pixel half-extent is the icon's
    // world half-size projected to the screen, clamped up to MinPixelRadius so a tiny/thin icon is
    // still a forgiving target. Offsetting in NDC at the center's depth keeps the proxy screen-
    // aligned and depth-correct (the clip.z drives the hardware depth test against scene geometry).
    float4 center = mul(view.CurViewProj, float4(bb.PositionSize.xyz, 1.0));

    float3 right = float3(view.View[0][0], view.View[0][1], view.View[0][2]);
    float4 edge = mul(view.CurViewProj, float4(bb.PositionSize.xyz + bb.PositionSize.w * 0.5 * right, 1.0));
    float ndcHalfX = abs(edge.x / edge.w - center.x / center.w);
    float pixelRadius = max(ndcHalfX * 0.5 * g_PC.RenderWidth, bb.MinPixelRadius);

    float2 ndcRadius = float2(pixelRadius * 2.0 / g_PC.RenderWidth,
                              pixelRadius * 2.0 / g_PC.RenderHeight);
    float2 ndcCenter = center.xy / center.w;
    float2 ndc = ndcCenter + corner * ndcRadius;

    VSOutput output;
    // Reapply w and the center depth so the corners share the center's clip-space depth.
    output.sv_position = float4(ndc * center.w, center.z, center.w);
    output.v_Quad = corner;
    output.v_PickId = bb.PickId;
    return output;
}
