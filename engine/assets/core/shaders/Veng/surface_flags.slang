// The g-buffer surface-flags channel (GBufferOutput::ORM alpha) — the flag values a material
// writes there and the predicates the deferred lighting pass reads them back with. It is its own
// header because both audiences need it and neither can include the other: Veng/surface.slang and
// deferred_lighting.frag.slang each declare the set-0 bindless arrays, so one including the other
// is a redeclaration.
//
// Encoding. The channel is the alpha of an RGBA8Unorm target and holds an eight-bit code, code / 255,
// which UNORM8 stores exactly and a decode reads back by rounding:
// - bit 0 declines shadow-map reception, bit 1 environment lighting;
// - bits 2-4 are the surface's light-channel membership (SurfaceLightChannels);
// - bit 5 declines the ambient arm's diffuse term, keeping its specular;
// - bit 6 is free;
// - bit 7 excludes the surface from the exposure meter. It is the highest bit because a Translucent
//   material raises it over the surface behind it through a MAX blend, which yields a value carrying
//   it only when it is worth more than every other bit together.
// Each flag's value is its bit's level, so flags combine by addition: write
// SurfaceFlagNoShadowReception + SurfaceFlagNoEnvironmentLighting for both, or SurfaceFlagsNone (0)
// for none.
//
// A reader takes the flags with a point load (LoadSurfaceFlags), never through a filtering sampler:
// no scalar encoding of independent flags survives a blend — halfway between "neither" and
// "both" is a level one of the other codes owns — so a tap straddling two surfaces would report a
// flag set neither carries.

#pragma once

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
static const float SurfaceFlagNoShadowReception = 1.0 / 255.0;

// Written into ORM.a by a material that is not lit by the view's sky: the image-based (split-sum
// IBL) and SH skylight ambient arms are replaced, for this surface alone, by the flat ambient floor
// the view carries. A surface whose environment is not the one the view's sky describes — an
// interior lit by its own fill, a body far from the probe the environment was captured at, an
// object the sky would tint wrongly — keeps a dim, uniform ambient instead, so in shadow it reads
// dim rather than black. Direct lights and their shadows are untouched.
static const float SurfaceFlagNoEnvironmentLighting = 2.0 / 255.0;

// Written into ORM.a by a material that supplies its own diffuse lighting: the ambient arm's diffuse
// term (the IBL irradiance, the SH skylight or the flat floor, whichever the surface takes) is
// dropped, while its specular term (the split-sum IBL reflection) is kept, scaled by occlusion as
// for any surface. A surface that writes its own sky-lit diffuse into emissive would otherwise be
// lit twice by the sky, and declining all ambient through an occlusion of zero would also take
// away its reflection of the environment. Direct lights and their shadows are untouched. Beside
// SurfaceFlagNoEnvironmentLighting the surface takes no ambient at all, since the flat floor has no
// specular term.
static const float SurfaceFlagNoAmbientDiffuse = 32.0 / 255.0;

// Written into ORM.a by a material whose emission is a source of light — a lamp, a light panel, a
// lit sign — that the auto-exposure meter does not read. The meter then exposes for the light such a
// surface casts on what is round it rather than for the surface itself, which may blow out, as a
// lamp does to a camera exposed for its room. Without it a few bright fixtures in view hold the
// exposure down (the highlight cap, or simply their weight in the average) and the room they light
// renders dark. The surface's own shading is untouched. A Translucent material raises it over the
// surface behind it through TranslucentMeteringOutput (Veng/translucent.slang).
static const float SurfaceFlagExcludedFromMetering = 128.0 / 255.0;

// The light-channel membership a surface carries by default: channel 0 alone, the channel every
// light lights unless it says otherwise (Light::Channels).
static const uint SurfaceLightChannelsDefault = 0x1u;

// The flags value placing a surface in the light channels mask names (bit c for channel c, three
// channels). A light lights only the surfaces sharing a channel with it, so a surface leaving
// channel 0 is skipped by every light on channel 0 alone, and one joining channel 1 is lit by the
// lights placed there. It adds to the other flags like any of them:
// SurfaceLightChannels(0x2u) + SurfaceFlagNoEnvironmentLighting.
//
// The membership is stored relative to the default, so a surface writing no flags at all is in
// channel 0 only: bit 2 marks a surface that leaves channel 0, bits 3 and 4 one that joins
// channels 1 and 2. SurfaceLightChannels(SurfaceLightChannelsDefault) is therefore 0.
float SurfaceLightChannels(uint mask)
{
    uint code = ((mask & 0x1u) == 0u ? 4u : 0u) + ((mask & 0x2u) != 0u ? 8u : 0u) +
                ((mask & 0x4u) != 0u ? 16u : 0u);
    return float(code) / 255.0;
}

// The eight-bit code a flags value holds (see "Encoding" above).
uint SurfaceFlagsCode(float ormAlpha)
{
    return uint(round(saturate(ormAlpha) * 255.0));
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

// True when a flags value carries SurfaceFlagNoAmbientDiffuse.
bool GBufferDeclinesAmbientDiffuse(float ormAlpha)
{
    return (SurfaceFlagsCode(ormAlpha) & 32u) != 0u;
}

// True when a flags value carries SurfaceFlagExcludedFromMetering.
bool GBufferExcludedFromMetering(float ormAlpha)
{
    return (SurfaceFlagsCode(ormAlpha) & 128u) != 0u;
}

// The light channels a flags value places its surface in, as a mask (bit c for channel c): the
// inverse of SurfaceLightChannels.
uint GBufferLightChannels(float ormAlpha)
{
    uint code = SurfaceFlagsCode(ormAlpha);
    return ((code & 4u) == 0u ? 0x1u : 0u) | ((code >> 2u) & 0x6u);
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
