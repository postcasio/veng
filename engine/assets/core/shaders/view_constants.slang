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
};

static const uint ViewConstantsStride = 512;

[[vk::binding(5, 0)]] ByteAddressBuffer g_ViewConstants;

ViewConstants LoadViewConstants(uint index)
{
    return g_ViewConstants.Load<ViewConstants>(index * ViewConstantsStride);
}
