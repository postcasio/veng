// The engine's forward light loop for Translucent materials: lights a translucent fragment the way
// the deferred pass lights an opaque pixel. A Translucent material `#include`s this in place of
// Veng/translucent.slang (it re-exports that contract whole), builds a ForwardSurface from its own
// parameters, and calls EvaluateForwardLighting — the same shared core (Veng/lighting.slang) the
// deferred pass runs, over the same per-view lights, shadows and ambient, so an alpha-blended
// surface (glass, a visor, a foliage card) sits in the scene's light like the opaque surfaces
// around it.
//
// Units: the returned radiance is in the same exposure-scaled linear HDR units the deferred pass
// writes, so a fragment returns it straight into the translucent pass's lit scene-color target.
//
// What matches the deferred pass, and what does not:
// - The direct lights (directional, point, spot, and the Rect / Sphere / Polygon area lights) and
//   their shadowing — directional cascades, the punctual atlas, and the PCSS area shadows — are
//   evaluated identically. The shadow maps hold only opaque casters, so a translucent surface is
//   shadowed by opaque geometry and never by another translucent one.
// - The ambient arm (split-sum IBL, SH skylight, or the flat floor) is the one the view's sky
//   selects, scaled by ForwardSurface.Occlusion. Screen-space AO is not applied: it is computed
//   from the opaque depth, which describes what lies behind a translucent surface, not the surface.
// - Emission is not part of the result; a fragment adds its own.
//
// Bindings: the loop reads the set-0 light and area-vertex buffers (bindings 6 and 7), the
// image-based-lighting maps at set 4, and the shadow system at set 5 — set 3 is the per-draw
// DrawData. A Translucent fragment that declares any binding in sets 4 or 5 is drawn by the
// translucent pass with the renderer's own layouts at those sets and has them bound, so sets 4 and
// 5 are reserved to this include in the Translucent domain; a translucent material that does not
// include it binds nothing extra and pays nothing.
//
// Diffuse and Specular come back apart because straight alpha weights the whole returned colour
// by coverage: a material that wants its reflections to survive at low opacity (clear glass is
// nearly all reflection) raises its coverage by the specular luminance, or divides the specular
// term by its coverage, rather than letting the blend dim both alike.
//
// Each component comes back through the lighting core's output guard (GuardLightingOutput), which
// keeps it inside the scene colour's half-float range. The guard bounds what the loop returns, not
// what a material writes: a material that scales either component up (dividing specular by its
// coverage multiplies it by the coverage's reciprocal) or sums them with other radiance passes its
// final colour through GuardLightingOutput before returning it.

#include "Veng/translucent.slang"

// Set 3 in a surface-drawn layout is the per-draw DrawData; the shadow system goes after the IBL
// set, where the translucent pass binds it.
#define VE_SHADOW_SET 5
#include "Veng/lighting.slang"

// The surface a forward-lit fragment shades, in world space.
struct ForwardSurface
{
    float3 Position;   // world position (reconstructable from sv_position via ForwardWorldPosition)
    float3 Normal;     // world-space shading normal, unit
    float3 View;       // unit vector from the surface toward the camera
    float3 Albedo;     // linear base colour
    float  Roughness;  // perceptual roughness [0,1] (floored at 0.04, as the deferred pass floors it)
    float  Metallic;   // [0,1]
    float  Occlusion;  // ambient occlusion [0,1]
};

// The light a forward-lit fragment receives, split so a material can weight the two apart.
struct ForwardLighting
{
    float3 Diffuse;    // diffuse radiance (direct + ambient), already multiplied by albedo terms
    float3 Specular;   // specular radiance (direct + ambient)
};

// Evaluates the scene's lights and ambient for a translucent fragment exactly as the deferred pass
// would for an opaque surface with the same parameters: every live light in this view (shadowed),
// plus the view's ambient arm scaled by s.Occlusion. Returns exposure-scaled linear HDR radiance,
// each component guarded; a caller that amplifies or sums them re-applies GuardLightingOutput to
// what it writes.
ForwardLighting EvaluateForwardLighting(ForwardSurface s)
{
    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);
    LightingSurface surface = MakeLightingSurface(vc, s.Position, s.Normal, s.View, s.Albedo,
                                                  s.Roughness, s.Metallic,
                                                  /*declinesShadowMaps=*/false);

    LightingResult direct = EvaluateDirectLighting(vc, surface);
    LightingResult ambient = EvaluateAmbientLighting(vc, surface, s.Occlusion);

    ForwardLighting lit;
    lit.Diffuse = GuardLightingOutput(direct.Diffuse + ambient.Diffuse);
    lit.Specular = GuardLightingOutput(direct.Specular + ambient.Specular);
    return lit;
}

// The world position of this fragment reconstructed from its SV_Position, through the same
// inverse view-projection the deferred pass reconstructs an opaque pixel's position with. Holds in
// the reduced-resolution translucent layer too: its view block carries the layer's own extents.
float3 ForwardWorldPosition(float4 svPosition)
{
    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);
    float2 ndcXY = SceneScreenUV(vc, svPosition) * 2.0 - 1.0;
    float4 worldH = mul(vc.InvViewProj, float4(ndcXY, svPosition.z, 1.0));
    return worldH.xyz / worldH.w;
}

// The unit vector from a world position toward this view's camera — ForwardSurface.View.
float3 ForwardViewVector(float3 worldPosition)
{
    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);
    return normalize(vc.CameraPosition.xyz - worldPosition);
}
