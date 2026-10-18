// The pre-translucent opaque-depth copy, read from any pass drawn after it: a soft edge where a
// translucent surface or a sprite meets geometry, a medium marched to the nearest surface, a
// refraction gate. The copy is a raw reverse-Z device depth published through the view block
// (SceneColor.w, gated by SceneDepthCopied); ViewDepthFromDeviceDepth turns a sample into a
// positive view-space depth comparable with the reader's own.
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
// so larger is nearer. A refracting material depth-gates a distorted sample against its own
// sv_position.z: a sampled depth nearer than the fragment belongs to geometry in front of the
// refractor, whose light never passed through it, so the sample is foreground contamination to
// reject. A medium or a soft edge reads it to find the surface behind the fragment. Point fields
// write no depth, so they are not in it. Returns the far plane (reverse-Z: 0) when the copy is
// unavailable.
float SampleSceneDepth(ViewConstants vc, float2 logicalUV)
{
    if (!SceneDepthAvailable(vc))
    {
        return 0.0;
    }
    const float2 uv = ScaledSampleUV(saturate(logicalUV), vc.RenderScaleUV.xy, vc.MaxValidUV.xy);
    return g_Textures[vc.SceneColor.w].SampleLevel(g_Samplers[vc.SceneColor.y], uv, 0.0).r;
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
