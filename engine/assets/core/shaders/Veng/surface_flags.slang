// The g-buffer surface-flags channel (GBufferOutput::ORM alpha) — the flag values a material
// writes there and the predicates the deferred lighting pass reads them back with. It is its own
// header because both audiences need it and neither can include the other: Veng/surface.slang and
// deferred_lighting.frag.slang each declare the set-0 bindless arrays, so one including the other
// is a redeclaration.
//
// The channel is the alpha of an RGBA8Unorm target, which round-trips a written 1.0 exactly. One
// flag is defined; a second gets its own value and its own predicate rather than a bit mask, so a
// reader never has to know how many flags exist to test the one it cares about.

// Written into ORM.a by a material whose drawn geometry does not stand where its shading says it
// does — an impostor, a sky proxy, a matte-painting shell, a proxy drawn at a compressed distance
// to hold an apparent size. The lighting pass reconstructs a world position for every fragment
// from the depth attachment, and for such a fragment that position is a coordinate no geometry in
// the scene has any relationship to, so a shadow map sampled there returns whatever silhouette
// unrelated geometry happens to cast across that region. The flag declines the lookup.
//
// It is the receiving half of the statement MeshRenderer::CastsShadows = false makes about
// casting, and it rides the material rather than the instance because "my drawn position is a
// proxy" is a property of how a surface shades: the same material is right for every mesh drawn
// through it, while a caster is a property of a draw. The fragment is still lit — the flag
// removes shadow-map occlusion and nothing else.
static const float SurfaceFlagNoShadowReception = 1.0;

// True when a sampled flags channel carries SurfaceFlagNoShadowReception. The half-way threshold
// guards against a filtered read rather than against precision: a UNORM8 round-trips 1.0 exactly,
// but the fullscreen passes read the g-buffer through a linear clamp sampler, so a tap straddling
// the boundary between a flagged and an unflagged fragment lands between the two values.
bool GBufferDeclinesShadowMaps(float ormAlpha)
{
    return ormAlpha >= 0.5;
}
