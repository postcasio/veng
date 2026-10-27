// The pre-translucent opaque-depth copy, read from any pass drawn after it: a soft edge where a
// translucent surface or a sprite meets geometry, a medium marched to the nearest surface, a
// refraction gate. The copy is a raw reverse-Z device depth published through the view block
// (SceneColor.w, gated by SceneDepthCopied); ViewDepthFromDeviceDepth turns a read into a
// positive view-space depth comparable with the reader's own.
//
// Three kinds of read, by what the result must be:
//   - SampleSceneDepth is filtered: a smooth value for a soft fade, but across a silhouette it
//     blends depths into one no surface has;
//   - LoadSceneDepth is a point load of the texel under a UV: a depth some surface wrote;
//   - SampleSceneDepthNearest / SampleSceneDepthFarthest take the nearest or farthest of the
//     copy texels a fragment's footprint covers: one texel at full resolution, the 2x2 in the
//     half-resolution layer. A ray march ends at one of these.
//
// The includer declares the view block (view_constants.slang) and the set-0 bindless arrays
// g_Textures / g_Samplers before including this. It declares no binding of its own, and
// ViewDepthFromProjection reads none, so a dispatch with no view bound may call it.

#pragma once

// A fragment's logical [0,1] screen UV from its SV_Position: the viewport covers the valid
// (rendered sub-rect) extent, so the divide is by ExtentParams.xy. Perturb this UV and pass
// it to SampleSceneColor to read the scene behind the fragment.
float2 SceneScreenUV(ViewConstants vc, float4 svPosition)
{
    return svPosition.xy / vc.ExtentParams.xy;
}

// True when the pre-translucent opaque-depth copy is populated this frame (a drawn material
// declares "readsSceneDepth" or "readsSceneColor"); SampleSceneDepth returns the far plane
// otherwise. No renderer setting gates it.
bool SceneDepthAvailable(ViewConstants vc)
{
    return vc.SceneDepthCopied != 0;
}

// Samples the pre-translucent opaque depth at a logical [0,1] screen UV — raw [0,1], reverse-Z
// so larger is nearer — through a linear filter. A refracting material depth-gates a distorted
// sample against its own sv_position.z: a sampled depth nearer than the fragment belongs to
// geometry in front of the refractor, whose light never passed through it, so the sample is
// foreground contamination to reject. A soft edge reads it to find the surface behind the
// fragment. Point fields write no depth, so they are not in it. Returns the far plane (reverse-Z:
// 0) when the copy is unavailable.
//
// The filter blends neighbouring texels wherever the UV is off a texel centre — under a render
// scale, and at every fragment of the half-resolution layer, whose UV falls on the corner of four.
// Across a silhouette that blend is a depth no surface has (the mean of a surface at distance D
// and the far plane reads about 2D), so a march's end point takes LoadSceneDepth or a footprint
// read instead.
float SampleSceneDepth(ViewConstants vc, float2 logicalUV)
{
    if (!SceneDepthAvailable(vc))
    {
        return 0.0;
    }
    const float2 uv = ScaledSampleUV(saturate(logicalUV), vc.RenderScaleUV.xy, vc.MaxValidUV.xy);
    return g_Textures[vc.SceneColor.w].SampleLevel(g_Samplers[vc.SceneColor.y], uv, 0.0).r;
}

// The copy's valid (rendered) sub-rect in its own texels. The copy is at the render allocation and
// holds the frame's sub-rect at its origin, so RenderScaleUV scales its dimensions to that rect.
int2 SceneDepthValidTexels(ViewConstants vc, Texture2D copy)
{
    uint width;
    uint height;
    copy.GetDimensions(width, height);
    return max(int2(round(vc.RenderScaleUV.xy * float2(width, height))), int2(1, 1));
}

// The opaque depth of the copy texel a logical [0,1] screen UV falls in, by a point load — a depth
// some surface wrote, at any render scale and resolution. Returns the far plane (reverse-Z: 0)
// when the copy is unavailable.
float LoadSceneDepth(ViewConstants vc, float2 logicalUV)
{
    if (!SceneDepthAvailable(vc))
    {
        return 0.0;
    }
    Texture2D copy = g_Textures[vc.SceneColor.w];
    const int2 valid = SceneDepthValidTexels(vc, copy);
    const int2 texel = clamp(int2(floor(saturate(logicalUV) * float2(valid))), int2(0, 0), valid - 1);
    return copy.Load(int3(texel, 0)).r;
}

// The first and last copy texels (inclusive, per axis) whose centres lie inside the footprint of a
// fragment centred at a logical UV on a target of `extent` pixels covering the same screen. A
// footprint smaller than a texel keeps the one texel the UV falls in.
void SceneDepthFootprint(int2 valid, float2 logicalUV, float2 extent, out int2 first,
                         out int2 last)
{
    const float2 halfSpan = 0.5 * float2(valid) / max(extent, float2(1.0, 1.0));
    const float2 centre = saturate(logicalUV) * float2(valid);
    first = clamp(int2(ceil(centre - halfSpan - 0.5)), int2(0, 0), valid - 1);
    last = clamp(int2(ceil(centre + halfSpan - 0.5)) - 1, first, valid - 1);
}

// The four copy depths of a fragment's footprint, read as the first two texels per axis of the
// range SceneDepthFootprint gives (repeating a texel where the range is one wide), so a target at
// least half the copy's resolution is covered exactly.
float4 LoadSceneDepthFootprint(ViewConstants vc, float2 logicalUV, float2 extent)
{
    Texture2D copy = g_Textures[vc.SceneColor.w];
    int2 first;
    int2 last;
    SceneDepthFootprint(SceneDepthValidTexels(vc, copy), logicalUV, extent, first, last);
    const int2 second = min(first + int2(1, 1), last);
    return float4(copy.Load(int3(first, 0)).r, copy.Load(int3(int2(second.x, first.y), 0)).r,
                  copy.Load(int3(int2(first.x, second.y), 0)).r, copy.Load(int3(second, 0)).r);
}

// The nearest (reverse-Z: largest) opaque depth over the copy texels a fragment covers, for a
// fragment at a logical UV on a target of `extent` pixels — vc.ExtentParams.xy for the drawing
// fragment's own target, which makes it one texel at full resolution and the 2x2 in the
// half-resolution layer. A march that must never pass through a near surface ends here. Returns
// the far plane (0) when the copy is unavailable.
float SampleSceneDepthNearest(ViewConstants vc, float2 logicalUV, float2 extent)
{
    if (!SceneDepthAvailable(vc))
    {
        return 0.0;
    }
    const float4 depths = LoadSceneDepthFootprint(vc, logicalUV, extent);
    return max(max(depths.x, depths.y), max(depths.z, depths.w));
}

// The farthest (reverse-Z: smallest) opaque depth over the copy texels a fragment covers; see
// SampleSceneDepthNearest for the footprint. The half-resolution layer depth-tests and composites
// against the farthest of each 2x2, and over an even valid extent this footprint is that 2x2 (over
// an odd one it sits up to a texel short of the layer's blocks along that axis's far half), so a
// half-resolution march that must agree with its composite ends here. Returns the far plane (0) when the copy is unavailable.
float SampleSceneDepthFarthest(ViewConstants vc, float2 logicalUV, float2 extent)
{
    if (!SceneDepthAvailable(vc))
    {
        return 0.0;
    }
    const float4 depths = LoadSceneDepthFootprint(vc, logicalUV, extent);
    return min(min(depths.x, depths.y), min(depths.z, depths.w));
}

// The positive view-space depth a device depth stands for under a projection, solved from its depth
// and w rows: device depth d = (A·z + B) / (C·z + D) for view-space z, so the depth in front of the
// camera is (d·D − B) / (d·C − A). That one form covers a reverse-Z perspective, finite or
// infinite (C = −1, D = 0), and an orthographic projection (C = 0, D = 1). Temporal jitter moves
// only the x and y rows, so a jittered projection gives the same answer. The far plane of an
// infinite perspective, a device depth of 0, reads back as +inf.
float ViewDepthFromProjection(float4x4 proj, float deviceDepth)
{
    const float a = proj[2][2];
    const float b = proj[2][3];
    const float c = proj[3][2];
    const float d = proj[3][3];
    return (deviceDepth * d - b) / (deviceDepth * c - a);
}

// The positive view-space depth of a device depth under this view's jittered projection.
float ViewDepthFromDeviceDepth(ViewConstants vc, float deviceDepth)
{
    return ViewDepthFromProjection(vc.Proj, deviceDepth);
}
