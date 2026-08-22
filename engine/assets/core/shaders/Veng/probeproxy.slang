// Reflection-proxy intersection for an environment capture: the correction that stops a captured
// environment being treated as infinitely distant. Bindings-free, so it is includable beside any
// domain contract, and composed with Veng/octahedral.slang rather than absorbing it — a consumer
// computes a corrected direction here and samples the capture at OctahedralUV(direction), so either
// module is useful without the other.
//
// **What the correction is.** A capture is a view of the scene from one point, the probe centre,
// while the ray that should be sampled leaves the shaded fragment. Sampling the map by the bare
// reflected ray answers "what would the probe see in that direction", which is right only for
// content infinitely far away; it is wrong by the whole extent of the reflected content when that
// content is close, and the error moves with the fragment. The correction intersects the reflected
// ray with a **proxy** — a simple stand-in for the geometry the capture actually holds — and takes
// the direction from the probe centre to that hit instead. A sphere stands in for a body; a box
// stands in for an enclosure, where the reflected surfaces are the walls of a cabin, a room, or a
// corridor.
//
// **The frame is the caller's business.** Both entry points take the ray *already probe-relative*
// (its origin measured from the probe centre) and *already expressed in the basis the proxy is
// authored in*. This module carries no quaternion and no axes and rotates nothing. A capture whose
// faces are world-aligned publishes the identity rotation, so a world-space ray needs no
// conversion; one oriented in a moving carrier's frame does, and a caller that skips it has a
// defect that looks right in every still image and only fails under rotation.
//
// **Both entry points are total.** They always return a direction — a grazing sphere miss clamps to
// the tangent point, an origin outside the box clamps to t = 0 — because a fragment shader has no
// useful miss branch. The miss-reporting form is Veng/slab.slang, for a caller that wants one.
//
// **The result is the unnormalized vector from the probe centre to the hit.** Normalizing is left
// to the caller, which normalizes at the OctahedralUV call anyway; returning a unit vector would
// buy a second normalize per shaded fragment for nothing.
//
// **A degenerate proxy disables the correction.** A radius at or below zero, and a box with a
// non-positive extent on any axis (an inverted box included), return the ray direction unchanged —
// the uncorrected direction, which is what an infinitely distant environment reads as and what a
// caller with no proxy at all would have sampled. It is never a division by zero and never a NaN,
// so a material may author a proxy off and get today's result rather than a broken one.

#ifndef VENG_PROBE_PROXY
#define VENG_PROBE_PROXY

#include "slab.slang"

// Smallest positive extent a box proxy needs on every axis to be treated as a proxy at all.
static const float ProbeProxyMinExtent = 1e-4;

// Smallest squared length a corrected direction may have before it is discarded as degenerate.
static const float ProbeProxyMinLengthSq = 1e-12;

// Returns the hit direction, or the ray direction when the hit landed on the probe centre.
//
// A hit at the centre is a zero vector, which normalize turns into a NaN and OctahedralUV into a
// black or garbage sample; the ray direction is the meaningful fallback, being the direction the
// caller would have sampled with no proxy.
float3 ProbeProxyGuardDirection(float3 hit, float3 dir)
{
    return dot(hit, hit) > ProbeProxyMinLengthSq ? hit : dir;
}

// Corrects a ray against a sphere proxy centred on the probe.
//
// Takes the far intersection: a ray starting inside the sphere — which a radius enclosing the
// reflected content guarantees — leaves it in front of the fragment, and that exit is the point the
// probe should be sampled toward. A ray starting outside, or one that misses the sphere entirely,
// clamps to the point of closest approach: the discriminant is floored at zero, which is the
// tangent point of the grazing ray.
//
// @param origin The ray origin, measured from the probe centre and in the capture's frame.
// @param dir    The ray direction in the same frame; need not be normalized.
// @param radius The proxy sphere's radius. At or below zero the correction is disabled.
// @return The unnormalized direction from the probe centre to the hit.
float3 ProbeProxySphereDirection(float3 origin, float3 dir, float radius)
{
    if (radius <= 0.0)
    {
        return dir;
    }

    // The quadratic |origin + t·dir|² = radius², solved for its larger root. Dividing by
    // a = |dir|² is what makes an unnormalized direction give the same hit point as a normalized
    // one; it is non-zero for any usable ray and floored so a zero direction cannot divide by zero.
    const float a = max(dot(dir, dir), ProbeProxyMinLengthSq);
    const float b = dot(origin, dir);
    const float c = dot(origin, origin) - radius * radius;
    const float t = (-b + sqrt(max(b * b - a * c, 0.0))) / a;

    return ProbeProxyGuardDirection(origin + dir * t, dir);
}

// Corrects a ray against a box proxy expressed in the probe's frame.
//
// Takes the slab test's exit parameter, floored at zero: a ray starting inside the enclosure leaves
// through the wall it is heading for, which is the surface the probe should be sampled toward, and
// a ray starting outside the box (or missing it) resolves to its own origin rather than to a point
// behind the fragment.
//
// @param origin The ray origin, measured from the probe centre and in the capture's frame.
// @param dir    The ray direction in the same frame; need not be normalized.
// @param boxMin The proxy box's lower corner, measured from the probe centre and in that frame.
// @param boxMax The proxy box's upper corner, in the same terms.
// @return The unnormalized direction from the probe centre to the hit.
float3 ProbeProxyBoxDirection(float3 origin, float3 dir, float3 boxMin, float3 boxMax)
{
    const float3 extent = boxMax - boxMin;
    if (min(min(extent.x, extent.y), extent.z) <= ProbeProxyMinExtent)
    {
        return dir;
    }

    const SlabHit slab = IntersectRayBox(origin, dir, boxMin, boxMax);
    const float t = max(slab.Exit, 0.0);

    return ProbeProxyGuardDirection(origin + dir * t, dir);
}

#endif
