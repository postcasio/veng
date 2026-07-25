// The single canonical per-frame view-constants block (set 0, binding 5): genuine
// camera/view state the deferred lighting pass, the SSAO pass, and materials read.
// World-position/view-position reconstruction (InvViewProj, Proj), the camera
// position, the view matrix for view-space depth and the SSAO hemisphere, and the
// previous and current unjittered view-projections (PrevViewProj / CurViewProj) the
// TAA resolve reprojects through and the surface stage writes its g-buffer velocity
// channel from. The directional-shadow system rides a separate set-1 ShadowConstants
// block, not here.
//
// Ring-buffered across frames-in-flight, byte-addressed at index * ViewConstantsStride;
// mirrors BindlessRegistry::ViewConstantsStride and the CPU ViewConstantsBlock exactly.
// Lights ride a separate ring-buffered light buffer at binding 6.
struct ViewConstants
{
    float4x4 InvViewProj;    // world-position reconstruction from depth (jittered under TAA)
    // Inverse of (Proj x rotation-only View): a translation-free view-direction reconstruction, for
    // a pass that wants the ray through a pixel rather than a point on it. Reconstructing a
    // direction from InvViewProj forms a far-plane world point and subtracts the camera off it —
    // two large near-equal numbers whose small difference is the answer — so it loses precision the
    // further the camera is from the world origin, unevenly across the frame. This has no
    // translation to cancel. Jittered with Proj, so it agrees with what was rasterized.
    float4x4 InvViewRotProj;
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

    // The cosine-convolved sky irradiance SH (order-2, 9 RGB coefficients) the dynamic
    // skylight ambient evaluates per fragment. Each element carries one RGB coefficient in
    // .xyz (.w is std140 padding); a vec4 array, not a vec3 array, so the C++ upload and the
    // shader read share one 16-byte stride and cannot drift. Valid only when the lighting
    // push SkylightOn is set; zeroed otherwise.
    float4 SkyShCoeffs[9];

    // The frame clock: x is wall-clock seconds since engine start (frame-locked — every view
    // and material in a frame reads one consistent value), y is the view's frame delta in
    // seconds, zw unused. An animating material (a scrolling texture, a pulsing emissive)
    // reads x rather than accumulating its own time. Zero in an offline context (the sky
    // cubemap bake) — a baked source is static by definition.
    float4 TimeParams;

    // The view's pixel extents: xy the valid (rendered sub-rect) extent, zw the allocation
    // extent. A per-submesh fragment derives its logical [0,1] screen UV as
    // sv_position.xy / ExtentParams.xy (the viewport covers the valid sub-rect), then maps
    // it to a physical sample UV through ScaledSampleUV — the screen-space read path a
    // fullscreen pass gets from its triangle UV.
    float4 ExtentParams;

    // The pre-translucent scene grab: x the bindless texture index of the lit opaque scene
    // color (lighting + sky + emissive, copied ahead of the translucent pass), y its sampler
    // index, z 1 when the copy ran this frame (the renderer's Refraction setting is on) and
    // 0 otherwise — a shader must gate on z before sampling x or w — and w the copied opaque
    // depth (raw [0,1], smaller nearer), so a refracting material can reject a distorted
    // sample whose geometry is in front of it. This is what lets a Translucent material
    // refract or distort the scene behind it; see Veng/translucent.slang for the helpers.
    uint4 SceneColor;
};

static const uint ViewConstantsStride = 640;

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
