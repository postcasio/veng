// Scene-light queries for a participating medium a Translucent material marches itself: volumetric
// fog with light shafts, dust or smoke lit through a window, any medium filling the view. Where
// Veng/forward_lighting.slang lights a translucent *surface* (a BRDF against a normal, once per
// fragment), this answers the two questions a medium asks at many points along its ray: which
// light is the one it wants (FindLightByTag), and how much of that light's shadow-mapped visibility
// reaches a world position (LightVisibility). It evaluates no BRDF, phase function or radiance;
// the material does, with the light record LoadLight returns.
//
// Include it after Veng/translucent.slang, whose view block and push block it reads. A material that
// also lights a surface includes Veng/forward_lighting.slang in place of Veng/translucent.slang (it
// re-exports that contract) and this after it; the two share the shadow system and light buffer.
//
// Bindings: the set-0 light buffer (binding 6, through Veng/light_buffer.slang), the shadow system
// at set 5 (through shadow.slang), and the IBL maps' declarations at set 4, which nothing here
// samples. A Translucent fragment declaring those sets is recognised as forward-lit and drawn with
// the renderer's own IBL and shadow sets bound, in the full- and the reduced-resolution translucent
// layer alike (see Veng/forward_lighting.slang, "Bindings").
//
// Everything here takes no screen-space derivative, so it is legal in non-uniform control flow
// and inside a loop: per march step, after a discard, in a branch some fragments of a quad skip.

#pragma once

// Set 3 in a surface-drawn layout is the per-draw DrawData; the shadow system goes at set 5, where
// the translucent pass binds it.
#ifdef VE_SHADOW_SET
#undef VE_SHADOW_SET
#endif
#define VE_SHADOW_SET 5
#include "../shadow.slang"
#include "Veng/light_buffer.slang"
#include "Veng/light_flags.slang"
// Nothing here reads the IBL maps, but a material's declared sets must be contiguous and the
// forward-lit layout binds set 4 ahead of the shadow set, so set 4 is declared all the same.
#include "Veng/ibl_maps.slang"

// The index FindLightByTag returns when no light of this view carries the tag.
static const uint LightIndexInvalid = 0xFFFFFFFFu;

// LightVisibility's filter: one hardware comparison tap (the comparison sampler's own 2×2
// bilinear footprint) per lookup. The choice for a medium integrating many points along a ray,
// where the march's own averaging softens the edge the single tap leaves.
static const uint LightVisibilitySingleTap = 0;

// LightVisibility's filter: the 3×3 comparison-tap PCF kernel the surface lookups use, nine taps
// per lookup (eighteen inside a cascade blend band).
static const uint LightVisibilityPcf = 1;

// The packed index of the first light in this view carrying tag (Light::Tag), or LightIndexInvalid
// when none does. Tag 0 is the untagged default and names no light, so FindLightByTag(vc, 0)
// returns LightIndexInvalid. Walks the view's live lights, at most the renderer's per-view cap of
// sixteen; a material finding the same light at every march step calls it once per fragment.
uint FindLightByTag(ViewConstants vc, uint tag)
{
    if (tag == 0u)
    {
        return LightIndexInvalid;
    }
    uint count = vc.LightState.y;
    for (uint i = 0; i < count; ++i)
    {
        if (LoadLightTag(vc, i) == tag)
        {
            return i;
        }
    }
    return LightIndexInvalid;
}

// The share of light lightIndex reaching worldPosition through the opaque shadow maps, in [0, 1]:
// 0 where an opaque caster stands between the point and the light, 1 where none does. It is
// visibility only — no range, cone or distance attenuation, which the material applies itself
// from the light record.
//
// It dispatches on the light's shadow arm exactly as the surface light loop does and samples
// through the same lookup core (Veng/lighting.slang, AccumulateLight), so a surface and a medium
// at the same point agree on whether it is shadowed; only the edge's softness differs:
// - a Directional reads the cascade set fit to it, selected by the point's own view depth;
// - a near-parallel area light reads its cascade set the same way, along its travel direction;
// - a point or spot light, or any other area light, reads its punctual atlas tile. An area light's
//   tile is read through the plain comparison lookup, never its PCSS blocker search, so the
//   result carries no per-fragment rotated pattern.
// The normal-offset bias the surface lookups take along a surface normal is taken along the
// direction to the light here: a medium has no normal, and stepping toward the light keeps a point
// on an occluder's lit face reading lit.
//
// Returns 1 when the point lies outside every shadow map's coverage (past the last cascade,
// outside a tile, behind a tile's projection), when the light casts no shadow (or was denied a
// cascade set), when shadows are off, and when lightIndex names no live light of this view — so a
// medium marched past the shadowed scene is lit there rather than black.
//
// filter is LightVisibilitySingleTap or LightVisibilityPcf.
float LightVisibility(ViewConstants vc, uint lightIndex, float3 worldPosition, uint filter)
{
    if (lightIndex >= vc.LightState.y)
    {
        return 1.0;
    }
    GpuLight light = LoadLight(vc, lightIndex);
    float flags = light.Cone.w;
    if (LightCastsNoShadow(flags))
    {
        return 1.0;
    }
    int pcfRadius = filter == LightVisibilityPcf ? ShadowPcfRadius : 0;
    float type = light.DirectionType.w;
    float viewDepth = -mul(vc.View, float4(worldPosition, 1.0)).z;

    bool isDirectional = type == LightTypeDirectional;
    bool isArea = type > LightTypeSpot + 0.5;
    if (isDirectional || (isArea && LightIsAreaCascadeShadowed(flags)))
    {
        if (LightIsCascadeDenied(flags))
        {
            return 1.0;
        }
        float3 L = normalize(-light.DirectionType.xyz);
        return DirectionalShadowVisibility(worldPosition, L, L, viewDepth, LightCascadeSet(flags),
                                           pcfRadius);
    }

    int slot = int(light.Cone.z);
    float3 toLight = light.PositionRange.xyz - worldPosition;
    float dist = length(toLight);
    if (slot < 0 || dist <= 1e-4)
    {
        return 1.0;
    }
    float3 L = toLight / dist;
    return PunctualShadowVisibility(worldPosition, L, L, slot, pcfRadius);
}

// LightVisibility through one hardware comparison tap (LightVisibilitySingleTap), the filter for
// a point integrated along a march.
float LightVisibility(ViewConstants vc, uint lightIndex, float3 worldPosition)
{
    return LightVisibility(vc, lightIndex, worldPosition, LightVisibilitySingleTap);
}
