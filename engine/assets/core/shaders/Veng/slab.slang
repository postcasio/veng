// Ray/axis-aligned-box slab intersection: the two ray parameters where a ray crosses a box, and
// whether it met the box at all. Bindings-free and single-purpose, so it is includable beside any
// domain contract — shared by the volume-field march, which clamps the segment it gets against its
// own ray origin and the scene depth, and by Veng/probeproxy.slang's box proxy, which takes the
// exit alone.
//
// The result is raw: both parameters may be negative, and nothing here clamps, discards, or
// normalizes. A caller applies the clamps its own path wants, which is the whole reason this is
// separate from the total entry points in Veng/probeproxy.slang.
//
// **The box and the ray must be expressed in the same basis.** This module carries no rotation and
// performs none; a caller with a box authored in some other frame crosses into that frame first.
//
// **The axis-parallel convention is the floored form.** A near-zero direction component has its
// magnitude floored before the reciprocal is taken, rather than relying on 1.0/0.0 yielding an IEEE
// infinity — a shader compile makes no such promise under the fast-math assumptions it may carry.
// The floored reciprocal is large enough that the parameter it produces loses the min/max selection
// exactly as an infinity would, so this form and the infinity form (which the CPU counterpart
// Veng/Renderer/VolumeMarch.h keeps) select the same faces for an axis-parallel ray and agree for
// every non-axis-parallel one. That is an equivalence of outcome, not of bits: nothing here claims
// a bit-level CPU/GPU mirror.

#ifndef VENG_SLAB
#define VENG_SLAB

// Smallest magnitude a ray-direction component keeps before its reciprocal is taken.
static const float SlabMinDirComponent = 1e-6;

// The ray parameters where a ray meets an axis-aligned box, and whether it met it.
struct SlabHit
{
    // Parameter of the near-face crossing. Negative when the origin is already past that face —
    // inside the box, or beyond it entirely.
    float Enter;
    // Parameter of the far-face crossing. Negative when the whole box is behind the origin.
    float Exit;
    // False when the ray misses the box, which leaves Enter and Exit meaningless.
    bool Hit;
};

// Intersects the ray origin + t·dir with the axis-aligned box [boxMin, boxMax], unclamped.
//
// dir need not be normalized: t is in units of |dir|, so the hit point origin + t·dir is exact for
// any non-zero direction. A box with an inverted bound on some axis is read as the box with that
// axis's bounds swapped, since the two parameters are sorted per axis before the selection — a
// defined outcome rather than a miss, and never a NaN.
//
// @param origin The ray origin, in the box's basis.
// @param dir    The ray direction, in the box's basis; need not be normalized.
// @param boxMin The box's lower corner.
// @param boxMax The box's upper corner.
// @return The entry/exit parameters and whether the ray met the box.
SlabHit IntersectRayBox(float3 origin, float3 dir, float3 boxMin, float3 boxMax)
{
    // Floor each component's magnitude while keeping its sign, so an axis-parallel ray divides by a
    // tiny number rather than by zero (see the header). step(0, x) is 1 at x == 0, so the sign of a
    // zero component is taken as positive; the parameter it yields is enormous either way and is
    // lost in the selection below, which is the same outcome as ignoring that axis.
    const float3 dirSign = step(float3(0.0), dir) * 2.0 - 1.0;
    const float3 invDir = dirSign / max(abs(dir), float3(SlabMinDirComponent));

    const float3 t0 = (boxMin - origin) * invDir;
    const float3 t1 = (boxMax - origin) * invDir;
    const float3 tSmall = min(t0, t1);
    const float3 tLarge = max(t0, t1);

    SlabHit hit;
    hit.Enter = max(max(tSmall.x, tSmall.y), tSmall.z);
    hit.Exit = min(min(tLarge.x, tLarge.y), tLarge.z);
    hit.Hit = hit.Exit >= hit.Enter;
    return hit;
}

#endif
