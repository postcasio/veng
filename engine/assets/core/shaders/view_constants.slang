// The single canonical per-frame view-constants block (set 0, binding 5): genuine
// camera/view state the deferred lighting pass, the SSAO pass, and materials read.
// World-position/view-position reconstruction (InvViewProj, Proj), the camera
// position, the view matrix for view-space depth and the SSAO hemisphere, and the
// previous and current unjittered view-projections (PrevViewProj / CurViewProj) the
// TAA resolve and velocity pass reproject through. The directional-shadow system
// rides a separate set-1 ShadowConstants block, not here.
//
// Ring-buffered across frames-in-flight, byte-addressed at index * ViewConstantsStride;
// mirrors BindlessRegistry::ViewConstantsStride and the CPU ViewConstantsBlock exactly.
// Lights ride a separate ring-buffered light buffer at binding 6.
struct ViewConstants
{
    float4x4 InvViewProj;    // world-position reconstruction from depth (jittered under TAA)
    float4   CameraPosition; // xyz; w unused
    float4x4 View;           // world → view (view-space depth, SSAO view space)
    float4x4 Proj;           // view → clip (jittered under TAA)
    float4x4 PrevViewProj;   // previous frame's unjittered world → clip (TAA reprojection)
    float4x4 CurViewProj;    // this frame's unjittered world → clip (TAA velocity)

    // Dynamic-resolution sub-rect mapping. Every render target is allocated at a high-water-mark
    // extent and rendered into its top-left valid sub-rect; a fullscreen pass samples an upstream
    // target's valid region by scaling its logical [0,1] UV by RenderScaleUV and clamping to
    // MaxValidUV (a half-texel inset, so a bilinear tap never bleeds past the valid edge). xy is
    // this frame's mapping; zw is the previous frame's (the TAA history was written then). At
    // scale 1.0 RenderScaleUV is 1 and the map is identity, so a full-resolution frame is unchanged.
    float4 RenderScaleUV; // xy this frame's validExtent/allocExtent, zw previous frame's
    float4 MaxValidUV;    // xy this frame's (validExtent-0.5)/allocExtent, zw previous frame's
};

static const uint ViewConstantsStride = 512;

[[vk::binding(5, 0)]] ByteAddressBuffer g_ViewConstants;

ViewConstants LoadViewConstants(uint index)
{
    return g_ViewConstants.Load<ViewConstants>(index * ViewConstantsStride);
}

// Maps a logical [0,1] screen UV to the physical UV of an upstream sub-rect render target,
// clamped to the valid region so a bilinear tap never samples past it. scaleUV is validExtent/
// allocExtent and maxUV is (validExtent-0.5)/allocExtent (this frame's xy, or the previous
// frame's zw for TAA history). Identity at scale 1.0.
float2 ScaledSampleUV(float2 logicalUV, float2 scaleUV, float2 maxUV)
{
    return min(logicalUV * scaleUV, maxUV);
}
