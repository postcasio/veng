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
// rgb and coverage in a — or a TranslucentDepthOutput below, which adds a fragment-written depth. The forward translucent pass draws it into the lit HDR scene-color target
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
