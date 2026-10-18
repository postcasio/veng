// The engine's Translucent material contract: a forward, alpha-blended surface domain that
// writes final HDR radiance directly (not the g-buffer). A Translucent material shares the
// canonical-layout vertex stage and the per-draw plumbing of the Surface domain — it is drawn
// per-submesh from the same gathered geometry, reads its per-draw record from the DrawData SSBO
// by the instance-rate candidate id, and reads its material selector from the v_MaterialOffset
// interpolant — so it `#include`s Veng/surface.slang for the set-0 bindless declarations, the
// per-frame view block, DrawData / LoadDrawData, the surface push block (g_PC), and the
// SurfaceFragmentInput struct. It declares its own MaterialParams beside this include, exactly as
// a Surface material does (the parameter block is per-shader — the cooker reflects each shader's
// own struct to pack its fields at the reflected offsets — so it is not part of this contract).
//
// A Translucent material's fragment stage outputs a single float4 SV_Target0: HDR linear color in
// rgb and coverage in a — or one of the structs below, which add a fragment-written depth
// (TranslucentDepthOutput), a bloom mask (TranslucentBloomOutput) or a reactive amount
// (TranslucentReactiveOutput, TranslucentBloomReactiveOutput). The forward translucent pass draws it
// into the lit HDR scene-color target after deferred lighting and the sky composite and before the
// bloom/tonemap tail, so translucents bloom and tonemap with the scene. The pass depth-TESTs against the opaque depth buffer with depth
// writes OFF (a translucent surface never occludes another), sorts submeshes back-to-front by view
// depth, and blends with STRAIGHT (non-premultiplied) alpha: the destination is weighted by
// (1 - a) and the source color by a, so a fragment returns its color un-premultiplied and lets the
// blend apply coverage. A fragment returns final radiance directly and is responsible for its own
// shading. To be lit by the scene's lights the way an opaque surface is, a fragment includes
// Veng/forward_lighting.slang in place of this file (it re-exports this contract) and calls
// EvaluateForwardLighting — the deferred pass's own light loop, shadows and ambient, fed the same
// per-view light state. A fragment that includes only this file runs no light loop; the set-0 view
// block (camera/view state) is available for one that shades itself.
//
// Refraction: when the renderer's Refraction setting is on, the lit opaque scene color
// (lighting + sky + emissive) is copied ahead of the translucent pass and reaches a fragment
// through the view block's SceneColor handles — so a translucent material can sample the
// scene behind itself at a perturbed screen UV (glass, water, heat haze, a lens). The copy
// predates the translucent pass, so one translucent surface never refracts another; the
// helpers below are the supported read path, and SceneColorAvailable gates the sample (the
// handles are unpopulated with the setting off). A material whose fragment samples the copy
// declares "readsSceneColor": true in its .vmat.json: the renderer runs the copy only on a frame
// some drawn material declares it, and on any other frame SceneColorAvailable is false.
//
// Scene depth: the opaque depth is copied beside the colour, and a material that reads only the
// depth — a soft fade where it meets geometry, a medium marched to the nearest surface — declares
// "readsSceneDepth": true instead. That records the one depth copy and none of the colour grab,
// and needs no Refraction setting; SceneDepthAvailable gates SampleSceneDepth. Such a material
// must not call the colour helpers: nothing at cook time stops it, and SceneColorAvailable is
// false on any frame no colour reader draws. A "readsSceneColor" material reads the depth too.
//
// The Surface, PostProcess, and Sky domains have their own contracts in Veng/surface.slang,
// Veng/postprocess.slang, and Veng/sky.slang.

#include "Veng/surface.slang"
// SceneScreenUV, SceneDepthAvailable, SampleSceneDepth and ViewDepthFromDeviceDepth.
#include "Veng/scene_depth.slang"

// The Translucent output with a fragment-written depth. A fragment returns this instead of a bare
// float4 SV_Target0 when the depth it should test against is not the depth its geometry
// rasterized to. The pass keeps depth writes off, so the written value decides this fragment's
// depth test and still occludes nothing; the draw loses early-Z rejection.
struct TranslucentDepthOutput
{
    float4 Color : SV_Target0;  // HDR linear color in rgb, coverage in a (un-premultiplied)
    float  Depth : SV_Depth;
};

// The Translucent output with a bloom mask. A fragment returns this instead of a bare float4
// SV_Target0 when the surface should bloom by a strength it names rather than by how bright it
// is — a display, an instrument face, an emissive decal, any surface a viewer should see glow
// without its core reading as white.
//
// Bloom's bright-pass normally derives its own mask from the filtered scene luminance, so the
// only way to raise a surface's glow is to raise the radiance it emits — and past the tone
// curve's shoulder that spends its hue instead, since every channel compresses toward the white
// point together. The mask breaks the two apart: level 0 of the bloom pyramid takes the larger
// of the luminance bright-pass and this mask, so a surface can stay inside the range the
// tonemapper renders in full saturation and still seed the pyramid at any strength, which makes
// the halo carry the surface's own color.
//
// **The mask is an amplitude, not a coverage fraction.** Level 0 multiplies the filtered color by
// it, so 1.0 seeds the pyramid with exactly the surface's own radiance and the values that buy a
// glow brighter than that radiance are the ones above 1. The target is a float format for that
// reason, and nothing on the path saturates it.
//
// The mask is a coverage-weighted quantity, because the pass blends the color it accompanies by
// that same coverage: a fragment covering a fraction of its pixel contributes that fraction of
// its color to the lit target, so it must contribute that fraction of its mask. A material that
// recovers a straight (un-premultiplied) color from a premultiplied source multiplies the mask
// back by coverage on the way out, or its antialiased edges glow less than its interiors.
//
// The mask target is cleared each frame and written only by materials that author it — a
// material declares `"bloomMask": true` and the pass enables the second attachment's writes for
// its pipeline alone, so a translucent material that returns a bare float4 is unaffected.
struct TranslucentBloomOutput
{
    float4 Color     : SV_Target0;  // HDR linear color in rgb, coverage in a (un-premultiplied)
    float  BloomMask : SV_Target1;  // bloom amplitude, weighted by the same coverage
};

// A Translucent material whose surface is thin, fast or animating — a field of moving motes, a flicker
// — may also write the temporal resolve's reactive mask: how much of the pixel it owns, in [0, 1], so
// TAA, TAAU and MetalFX take more of the current frame there instead of a history that smears or drops
// it. It declares `"reactive": true` and returns one of the structs below; the reactive amount is
// always SV_Target2, the bloom mask SV_Target1 or absent. The amounts of overlapping surfaces sum,
// saturating at 1. Only the full-resolution scene layer writes the mask: the cook refuses "reactive"
// beside "resolution": "half" or "placement": "postResolve". Large surfaces (glass, an atmosphere)
// should not declare it, since a reactive pixel keeps little of its history and shimmers as it would
// without a temporal resolve.
//
// Slang numbers outputs in declaration order unless every one carries an explicit location, so these
// structs pin each output to its SV_Target index; an author's own struct leaving a gap must do the
// same, and the cook refuses one that does not.
struct TranslucentReactiveOutput
{
    [[vk::location(0)]] float4 Color    : SV_Target0;  // HDR linear color in rgb, coverage in a
    [[vk::location(2)]] float  Reactive : SV_Target2;  // the reactive amount, in [0, 1]
};

struct TranslucentBloomReactiveOutput
{
    [[vk::location(0)]] float4 Color     : SV_Target0;  // HDR linear color in rgb, coverage in a
    [[vk::location(1)]] float  BloomMask : SV_Target1;  // bloom amplitude, weighted by coverage
    [[vk::location(2)]] float  Reactive  : SV_Target2;  // the reactive amount, in [0, 1]
};

// The reactive amount of a straight color: its coverage, or the luminance of the light it adds where
// an additive surface carries more light than coverage.
float TranslucentReactiveAmount(float4 color)
{
    return saturate(max(color.a, dot(color.rgb * color.a, float3(0.2126, 0.7152, 0.0722))));
}

// The light a surface returns, divided by the exposure the tonemap applies to this view, so it
// shows as it would at an exposure of 1 — an instrument or a display that must keep its authored
// brightness while auto-exposure follows the scene. Scale the colour only: the coverage, and any
// bloom-mask amplitude, are the surface's own.
float3 CompensateExposure(ViewConstants vc, float3 radiance)
{
    return radiance * ViewInverseExposure(vc);
}

// True when the pre-translucent scene-color copy is populated this frame (the renderer's
// Refraction setting is on and a drawn material declares "readsSceneColor"); SampleSceneColor
// returns black otherwise.
bool SceneColorAvailable(ViewConstants vc)
{
    return vc.SceneColor.z != 0;
}

// Samples the pre-translucent scene color at a logical [0,1] screen UV, mapping through the
// dynamic-resolution sub-rect (ScaledSampleUV clamps to the valid region, so a distorted UV
// never bleeds past the rendered edge). Returns black when the copy is unavailable.
float3 SampleSceneColor(ViewConstants vc, float2 logicalUV)
{
    if (!SceneColorAvailable(vc))
    {
        return float3(0.0, 0.0, 0.0);
    }
    const float2 uv = ScaledSampleUV(saturate(logicalUV), vc.RenderScaleUV.xy, vc.MaxValidUV.xy);
    return g_Textures[vc.SceneColor.x].SampleLevel(g_Samplers[vc.SceneColor.y], uv, 0.0).rgb;
}

// How many levels the scene-color grab carries: 1 when the renderer's RefractionBlur setting is
// off, and the depth of the halving chain when it is on. A material can branch on this to tell a
// blur it can have from one it cannot, though it never has to — asking for a blur without the
// chain simply returns the sharp sample.
uint SceneColorLevels(ViewConstants vc)
{
    return max(vc.SceneColorMipCount, 1u);
}

// Samples the pre-translucent scene color *blurred*, at a logical [0,1] screen UV.
//
// `blur` is in [0,1] across whatever chain the frame has: 0 is the sharp copy and 1 is the coarsest
// level, so a material authors an appearance rather than a texel count and it holds at any
// resolution and any render scale. With no chain this is SampleSceneColor.
//
// **The clamp widens with the level.** A coarse texel covers 2^lod fine ones, so the half-texel
// inset that keeps a bilinear tap inside the rendered sub-rect has to grow with it — otherwise a
// blurred sample near the edge of a dynamic-resolution frame pulls in the cleared region beyond it,
// which reads as the scene darkening toward the border.
float3 SampleSceneColorBlurred(ViewConstants vc, float2 logicalUV, float blur)
{
    if (!SceneColorAvailable(vc))
    {
        return float3(0.0, 0.0, 0.0);
    }
    const float lod = saturate(blur) * float(SceneColorLevels(vc) - 1u);
    const float2 inset = (exp2(lod) - 1.0) * 0.5 / max(vc.ExtentParams.zw, float2(1.0, 1.0));
    const float2 uv = ScaledSampleUV(saturate(logicalUV), vc.RenderScaleUV.xy,
                                     max(vc.MaxValidUV.xy - inset, float2(0.0, 0.0)));
    return g_Textures[vc.SceneColor.x].SampleLevel(g_Samplers[vc.SceneColor.y], uv, lod).rgb;
}
