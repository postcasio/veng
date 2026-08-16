// The engine's Translucent material contract: a forward, alpha-blended surface domain that
// writes final HDR radiance directly (not the g-buffer). A Translucent material shares the
// canonical-layout vertex stage and the per-draw plumbing of the Surface domain — it is drawn
// per-submesh from the same gathered geometry, reads its per-draw record from the DrawData SSBO
// by the instance-rate candidate id, and reads its material selector from the v_MaterialIndex
// interpolant — so it `#include`s Veng/surface.slang for the set-0 bindless declarations, the
// per-frame view block, DrawData / LoadDrawData, the surface push block (g_PC), and the
// SurfaceFragmentInput struct. It declares its own MaterialParams beside this include, exactly as
// a Surface material does (the parameter block is per-shader — the cooker reflects each shader's
// own struct to pack its fields at the reflected offsets — so it is not part of this contract).
//
// A Translucent material's fragment stage outputs a single float4 SV_Target0: HDR linear color in
// rgb and coverage in a — or one of the structs below, which add a fragment-written depth
// (TranslucentDepthOutput) or a bloom mask (TranslucentBloomOutput). The forward translucent pass draws it into the lit HDR scene-color target
// after deferred lighting and the sky composite and before the bloom/tonemap tail, so translucents
// bloom and tonemap with the scene. The pass depth-TESTs against the opaque depth buffer with depth
// writes OFF (a translucent surface never occludes another), sorts submeshes back-to-front by view
// depth, and blends with STRAIGHT (non-premultiplied) alpha: the destination is weighted by
// (1 - a) and the source color by a, so a fragment returns its color un-premultiplied and lets the
// blend apply coverage. A fragment returns final radiance directly — it is responsible for its own
// shading, and the engine runs no forward light loop for it; the set-0 view block (camera/view
// state) is available for a fragment that shades itself.
//
// Refraction: when the renderer's Refraction setting is on, the lit opaque scene color
// (lighting + sky + emissive) is copied ahead of the translucent pass and reaches a fragment
// through the view block's SceneColor handles — so a translucent material can sample the
// scene behind itself at a perturbed screen UV (glass, water, heat haze, a lens). The copy
// predates the translucent pass, so one translucent surface never refracts another; the
// helpers below are the supported read path, and SceneColorAvailable gates the sample (the
// handles are unpopulated with the setting off).
//
// The Surface, PostProcess, and Sky domains have their own contracts in Veng/surface.slang,
// Veng/postprocess.slang, and Veng/sky.slang.

#include "Veng/surface.slang"

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
    float  BloomMask : SV_Target1;  // [0,1] bloom strength, weighted by the same coverage
};

// True when the pre-translucent scene-color copy is populated this frame (the renderer's
// Refraction setting is on); SampleSceneColor returns black otherwise.
bool SceneColorAvailable(ViewConstants vc)
{
    return vc.SceneColor.z != 0;
}

// A fragment's logical [0,1] screen UV from its SV_Position: the viewport covers the valid
// (rendered sub-rect) extent, so the divide is by ExtentParams.xy. Perturb this UV and pass
// it to SampleSceneColor to read the scene behind the fragment.
float2 SceneScreenUV(ViewConstants vc, float4 svPosition)
{
    return svPosition.xy / vc.ExtentParams.xy;
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

// Samples the pre-translucent opaque depth at a logical [0,1] screen UV — raw [0,1], reverse-Z
// so larger is nearer, copied beside the scene color. A refracting material depth-gates a
// distorted sample against its own sv_position.z: a sampled depth nearer than the fragment
// belongs to geometry in front of the refractor, whose light never passed through it, so the
// sample is foreground contamination to reject. Point fields write no depth, so they cannot be
// gated. Returns the far plane (reverse-Z: 0) when the copy is unavailable.
float SampleSceneDepth(ViewConstants vc, float2 logicalUV)
{
    if (!SceneColorAvailable(vc))
    {
        return 0.0;
    }
    const float2 uv = ScaledSampleUV(saturate(logicalUV), vc.RenderScaleUV.xy, vc.MaxValidUV.xy);
    return g_Textures[vc.SceneColor.w].SampleLevel(g_Samplers[vc.SceneColor.y], uv, 0.0).r;
}
