// The octahedral distance map's sky sentinel and its meaning. A SceneCapture publishes, beside its
// octahedral radiance map, an octahedral map of the radial distance from the probe centre to the
// nearest surface in each direction, in world units (see SceneCapture). A direction in which the
// capture saw no geometry — the far-plane sky — has no distance, and its texel carries this
// sentinel rather than a real length.
//
// A large finite value, not an infinity: it composes with point filtering and with the ordinary
// less-than comparisons a distance consumer makes, where an IEEE infinity interacts badly with
// later filtering and with the fast-math assumptions a shader compile may carry. It is far beyond
// any world-unit distance a capture's far plane admits, so a real hit never reaches it and a
// consumer reads `>=` it as "no geometry, infinitely far". The storage is a single-channel 32-bit
// float (R32Sfloat), whose range holds the sentinel with room to spare.
//
// The C++ mirror is SceneCapture::DistanceSkySentinel — the two must not drift.
static const float CaptureDistanceSky = 1.0e30;

// Whether an octahedral distance-map sample is the sky sentinel (no geometry in that direction)
// rather than a real radial distance.
bool CaptureDistanceIsSky(float distance)
{
    return distance >= CaptureDistanceSky;
}
