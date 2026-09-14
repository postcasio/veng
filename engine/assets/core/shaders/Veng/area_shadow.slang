// The source-size policy behind the area-light PCSS estimator: how large the emitter is taken to
// be when it sizes a shadow, and the blocker-search and filter radii that size drives. Named no
// binding and no global, so the shadow header and a test shader evaluate the identical policy.
//
// The size a shadow is built from is NOT the emitter radius the LTC lighting integral uses. The
// lighting radius is a physical quantity and is correct at any size; this one is an input to an
// approximation with a limited domain, and is capped to stay inside it.

// The largest source angular radius — as a sine, emitter radius over receiver-to-light distance —
// the PCSS estimator is allowed to see. Past it a source still shadows; it is simply treated as
// subtending this much, which yields a softer shadow than physics would give.
//
// PCSS averages the depth of the blockers found in one search disc and then treats them as a
// single planar occluder shared by the whole emitter. Two points on opposite edges of the emitter
// view the scene along directions differing by twice the source's angular radius, so that single
// averaged blocker describes what both of them see only while the spread stays small against the
// angular structure of the occluding geometry. 0.25 puts the limit at an angular radius of ~14.5°,
// a ~29° spread, and in this estimator's own terms it holds the blocker search at 7.5 texels — a
// local neighbourhood of the receiver's shadow texel, where the averaged depth still describes
// geometry the receiver is near. Uncapped, a source as wide as its distance drives the search to
// 25.5 texels, past the filter's own 24-texel clamp: the average then summarises occluders the
// filter can never sample, and an opening a few texels wide is averaged away into a blocker plane
// that reports a receiver standing in full light as fully occluded.
static const float AreaShadowMaxSourceSin = 0.25;

// The source's angular radius as the estimator uses it: the emitter radius over the distance from
// the receiver to the light, capped at AreaShadowMaxSourceSin. The denominator is a distance to
// the light and nothing else — a shadow view's fitted far plane is not one.
float AreaShadowSourceSizeFactor(float lightRadius, float lightDistance)
{
    return min(lightRadius / max(lightDistance, 1e-4), AreaShadowMaxSourceSin);
}

// Blocker-search radius in shadow texels. The floor keeps a point source searching its own texel
// neighbourhood rather than a single tap.
float AreaShadowSearchTexels(float sizeFactor)
{
    return 1.5 + sizeFactor * 24.0;
}

// Variable filter radius in shadow texels: the receiver-to-blocker gap scaled by the source size,
// clamped to the widest disc the 16-tap Poisson filter resolves without breaking into dither.
float AreaShadowFilterTexels(float penumbra, float sizeFactor)
{
    return clamp(1.0 + penumbra * sizeFactor * 64.0, 1.0, 24.0);
}
