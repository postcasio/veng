// The g-buffer surface-flags channel (GBufferOutput::ORM alpha) — the flag values a material
// writes there and the predicates the deferred lighting pass reads them back with. It is its own
// header because both audiences need it and neither can include the other: Veng/surface.slang and
// deferred_lighting.frag.slang each declare the set-0 bindless arrays, so one including the other
// is a redeclaration.
//
// Encoding. The channel is the alpha of an RGBA8Unorm target and holds a three-bit code spread over
// eight evenly spaced levels, code / 7. UNORM8 stores each within half a step of its level, and a
// decode rounds to the nearest level, so it holds against any error short of a fourteenth of the
// range. Each flag's value is its bit's level, so flags combine by addition: write
// SurfaceFlagNoShadowReception + SurfaceFlagNoEnvironmentLighting for both, or SurfaceFlagsNone (0)
// for none.
//
// A reader takes the flags with a point load (LoadSurfaceFlags), never through a filtering sampler:
// no scalar encoding of independent flags survives a blend — halfway between "neither" and
// "both" is a level one of the other codes owns — so a tap straddling two surfaces would report a
// flag set neither carries.

// The value of a channel carrying no flag: the ordinary shading path.
static const float SurfaceFlagsNone = 0.0;

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
static const float SurfaceFlagNoShadowReception = 1.0 / 7.0;

// Written into ORM.a by a material that is not lit by the view's sky: the image-based (split-sum
// IBL) and SH skylight ambient arms are replaced, for this surface alone, by the flat ambient floor
// the view carries. A surface whose environment is not the one the view's sky describes — an
// interior lit by its own fill, a body far from the probe the environment was captured at, an
// object the sky would tint wrongly — keeps a dim, uniform ambient instead, so in shadow it reads
// dim rather than black. Direct lights and their shadows are untouched.
static const float SurfaceFlagNoEnvironmentLighting = 2.0 / 7.0;

// Written into ORM.a by a material whose emission is a source of light — a lamp, a light panel, a
// lit sign — that the auto-exposure meter does not read. The meter then exposes for the light such a
// surface casts on what is round it rather than for the surface itself, which may blow out, as a
// lamp does to a camera exposed for its room. Without it a few bright fixtures in view hold the
// exposure down (the highlight cap, or simply their weight in the average) and the room they light
// renders dark. The surface's own shading is untouched. A Translucent material raises it over the
// surface behind it through TranslucentMeteringOutput (Veng/translucent.slang).
static const float SurfaceFlagExcludedFromMetering = 4.0 / 7.0;

// The three-bit code a flags value holds: bit 0 declines shadow maps, bit 1 environment lighting,
// bit 2 the exposure meter.
uint SurfaceFlagsCode(float ormAlpha)
{
    return uint(round(saturate(ormAlpha) * 7.0));
}

// True when a flags value carries SurfaceFlagNoShadowReception.
bool GBufferDeclinesShadowMaps(float ormAlpha)
{
    return (SurfaceFlagsCode(ormAlpha) & 1u) != 0u;
}

// True when a flags value carries SurfaceFlagNoEnvironmentLighting.
bool GBufferDeclinesEnvironmentLighting(float ormAlpha)
{
    return (SurfaceFlagsCode(ormAlpha) & 2u) != 0u;
}

// True when a flags value carries SurfaceFlagExcludedFromMetering.
bool GBufferExcludedFromMetering(float ormAlpha)
{
    return (SurfaceFlagsCode(ormAlpha) & 4u) != 0u;
}

// The flags value of the ORM texel under a UV, read by a point load of the texel the UV falls in,
// so it is the value one surface wrote. The UV is in the texture's own space (after any
// dynamic-resolution remap).
float LoadSurfaceFlags(Texture2D orm, float2 uv)
{
    uint width;
    uint height;
    orm.GetDimensions(width, height);
    int2 last = int2(width, height) - 1;
    int2 texel = clamp(int2(floor(uv * float2(width, height))), int2(0), last);
    return orm.Load(int3(texel, 0)).a;
}
