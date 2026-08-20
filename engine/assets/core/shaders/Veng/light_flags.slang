// The GpuLight flags word (Cone.w) — the bit meanings the renderer packs there and the
// accessors the deferred lighting pass reads them back with. Mirrors the LightFlags constants
// in Veng/Renderer/LightPacking.h; the two must move together.
//
// The word is a small integer carried in a float, exact well past the four bits used here.
// A two-sided bit, an arm bit for area lights, a two-bit cascade-set index and a denial bit —
// which together say, for any light, whether it is shadowed and by which atlas.

// The area light emits from both faces of its shape.
static const int LightFlagTwoSided = 1;

// A near-parallel area light shadowed by the cascade atlas rather than by its punctual tile.
// A Directional is cascade-shadowed by its type and never carries this; an area light's arm
// cannot be read off its type, which is why the flag exists.
static const int LightFlagAreaCascadeShadowed = 2;

// Shift and mask of the cascade-set index: which of the atlas's stacked cascade sets was fit to
// this light's direction.
static const int LightFlagCascadeSetShift = 2;
static const int LightFlagCascadeSetMask = 3 << LightFlagCascadeSetShift;

// The light asked for a cascade set and the atlas had none left, so it shades unshadowed. Set
// only on a Directional, which has no punctual fallback — an area light denied a set keeps its
// own perspective tile instead.
static const int LightFlagCascadeDenied = 16;

// True when an area light emits from both faces of its shape.
bool LightIsTwoSided(float flags)
{
    return (int(flags) & LightFlagTwoSided) != 0;
}

// True when a near-parallel area light samples the cascade atlas instead of its punctual tile.
bool LightIsAreaCascadeShadowed(float flags)
{
    return (int(flags) & LightFlagAreaCascadeShadowed) != 0;
}

// True when a Directional was denied a cascade set and must shade unshadowed.
bool LightIsCascadeDenied(float flags)
{
    return (int(flags) & LightFlagCascadeDenied) != 0;
}

// The cascade set fit to this light's direction. Meaningful only for a light the cascade arm
// shadows; a denied or punctual light never reaches a cascade sample with it.
uint LightCascadeSet(float flags)
{
    return uint((int(flags) & LightFlagCascadeSetMask) >> LightFlagCascadeSetShift);
}
