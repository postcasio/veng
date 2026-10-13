// Scene-light queries for a participating medium a Translucent material marches itself: volumetric
// fog with light shafts, dust or smoke lit through a window, any medium filling the view. Where
// Veng/forward_lighting.slang lights a translucent *surface* (a BRDF against a normal, once per
// fragment), this answers the two questions a medium asks at many points along its ray: which
// light is the one it wants (FindLightByTag), and how much of that light's shadow-mapped visibility
// reaches a world position (LightVisibility). It evaluates no BRDF, phase function or radiance;
// the material does, with the light record LoadLight returns. LightShadowMapAt says which map
// that visibility is read through at a point and how finely it resolves there, so a march can
// space its lookups by the map's own texels.
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

// The view depth LightShadowMapAt reports for a map that view depth does not select: a punctual
// tile, which covers what its projection holds at any depth.
static const float LightShadowMapUnbounded = 3.0e38;

// The shadow map LightVisibility reads a light through at one point, as a medium spacing its
// lookups along a ray needs it. The map resolves nothing finer than a texel across the direction
// it projects along, so lookups about a texel apart along the ray's image in the map read all it
// holds, and the image's length is the ray's length times the sine of its angle to TowardLight,
// over TexelSize.
struct LightShadowMap
{
    // The unit direction toward the light that the map projects along at the point.
    float3 TowardLight;
    // The world units one of the map's texels spans at the point, across TowardLight; zero where
    // no map covers the point, which LightVisibility reads as lit.
    float TexelSize;
    // The view depth out to which the same map covers: a cascade's far split, past which the next
    // cascade (or none) takes over; LightShadowMapUnbounded for a punctual tile.
    float CoveredDepth;
};

// The map LightVisibility reads lightIndex through at worldPosition, chosen by the same arm and
// cascade select. A cascade's texel size is its fit's own; a punctual tile's is measured by
// projecting the point and one beside it across the light's direction, so it is the tile's
// perspective footprint at the point's depth. Zero TexelSize wherever LightVisibility would
// return 1 without sampling: no live light, no shadow cast or a cascade set denied, shadows off,
// past the last cascade, or outside a punctual tile's frustum.
LightShadowMap LightShadowMapAt(ViewConstants vc, uint lightIndex, float3 worldPosition)
{
    LightShadowMap map;
    map.TowardLight = float3(0.0, 1.0, 0.0);
    map.TexelSize = 0.0;
    map.CoveredDepth = 0.0;
    if (lightIndex >= vc.LightState.y)
    {
        return map;
    }
    GpuLight light = LoadLight(vc, lightIndex);
    float flags = light.Cone.w;
    float type = light.DirectionType.w;
    bool isDirectional = type == LightTypeDirectional;
    bool isArea = type > LightTypeSpot + 0.5;
    if (isDirectional || (isArea && LightIsAreaCascadeShadowed(flags)))
    {
        map.TowardLight = normalize(-light.DirectionType.xyz);
        uint count = uint(g_Shadow.ShadowParams.z);
        if (LightCastsNoShadow(flags) || LightIsCascadeDenied(flags) ||
            g_Shadow.ShadowParams.w < 0.5 || count == 0u)
        {
            return map;
        }
        uint set = LightCascadeSet(flags);
        float viewDepth = -mul(vc.View, float4(worldPosition, 1.0)).z;
        uint cascade = SelectCascade(viewDepth, count, set);
        if (cascade < count)
        {
            map.TexelSize = g_Shadow.Sets[set].TexelSize[cascade];
            map.CoveredDepth = g_Shadow.Sets[set].Splits[cascade];
        }
        return map;
    }

    float3 toLight = light.PositionRange.xyz - worldPosition;
    float dist = length(toLight);
    if (dist <= 1e-4)
    {
        return map;
    }
    float3 L = toLight / dist;
    map.TowardLight = L;
    int slot = int(light.Cone.z);
    if (LightCastsNoShadow(flags) || slot < 0)
    {
        return map;
    }
    PunctualShadowRecord record = g_Punctual.Records[slot];
    if (record.Params.x < 0.5)
    {
        return map;
    }
    uint face = record.Params.x > 1.5 ? 0u : SelectCubeFace(-L);
    float4 clip = mul(record.ViewProj[face], float4(worldPosition, 1.0));
    float3 up = abs(L.y) < 0.9 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 across = normalize(cross(L, up));
    float reach = 1e-3 * dist;
    float4 beside = mul(record.ViewProj[face], float4(worldPosition + across * reach, 1.0));
    if (clip.w <= 0.0 || beside.w <= 0.0)
    {
        return map;
    }
    float3 ndc = clip.xyz / clip.w;
    float2 uv = ndc.xy * 0.5 + 0.5;
    float2 tileSize = float2(1.0 / float(CubeFaceCount), 1.0 / float(MaxShadowedPunctual));
    float2 tileMin = float2(float(face), float(slot)) * tileSize;
    if (ndc.z < 0.0 || ndc.z > 1.0 || any(uv < tileMin) || any(uv > tileMin + tileSize))
    {
        return map;
    }
    // In texels of the tile, whose atlas-UV texel is anisotropic (PunctualShadowVisibility).
    float2 texel = g_Punctual.AtlasParams.x * tileSize;
    float texels = length((beside.xy / beside.w * 0.5 + 0.5 - uv) / texel);
    if (texels > 0.0)
    {
        map.TexelSize = reach / texels;
        map.CoveredDepth = LightShadowMapUnbounded;
    }
    return map;
}
