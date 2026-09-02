// A ray march against a SceneCapture's octahedral distance map: given a ray already expressed in the
// capture's frame and already measured from the probe centre, walk it against the recorded radial
// distance and return the direction of the surface the ray actually crosses. This is the parallax
// correction a capture-consuming material applies to its reflected (or refracted) ray before it
// samples the radiance map — the step immediately before OctahedralUV, for the same consumers, so it
// lives beside Veng/octahedral.slang and composes with it rather than absorbing it.
//
// A capture's radiance map is a view from one point, so sampling it by the bare reflection direction
// places every reflected surface at infinity — correct only when the reflected content is genuinely
// far. The distance map (Veng/capture_distance.slang) records how far away each direction's surface
// actually is, and this march turns that into the direction from the probe centre to the crossing,
// which is where the reflected content really sits.
//
// **The contract, stated so no call site has to guess:**
//
//  - `rayOrigin` is measured FROM the probe centre, and `rayDir` (need not be unit — it is
//    normalized here) is a direction, both expressed in the capture's frame. This module carries no
//    quaternion and performs no rotation: a World-aligned capture's frame is world space, so a
//    world-space ray needs no transform; an Entity-aligned capture's caller first rotates the ray by
//    the conjugate of CaptureSurface::OrientationSlot. This is forced, not chosen — the map is stored
//    in the capture's frame, so the corrected direction must come out in that frame for OctahedralUV.
//  - The distance map and its sampler are PARAMETERS, not bindings. This module names no descriptor
//    set, which is what keeps it includable beside any domain contract (the reason
//    Veng/octahedral.slang is bindings-free too).
//  - The result is the direction from the probe centre to the crossing, UNNORMALIZED, leaving
//    OctahedralUV and the radiance sample to the caller.
//  - **A miss returns the bare `rayDir`, and a miss is a DEFINED outcome, not a failure branch.** A
//    ray that crosses no recorded surface within its budget, or that passes only through sky-sentinel
//    directions, is looking at the sky, and the sky is correctly sampled by direction. A reader's
//    instinct is to treat this as an error; it is not.
//  - **The step budget is the caller's**, defaulting to CaptureDepthMarchSteps. An interior a metre
//    across and an environment kilometres across want different budgets and this module cannot know
//    which it has.
//
// **Linear search then binary refine.** Walk the ray in `steps` uniform increments, comparing the
// distance travelled from the probe centre against the distance the map records in the current
// direction; the crossing is the first increment where travelled first meets or exceeds recorded.
// Then bisect between the last two increments to land on the surface. The failure mode this shape
// carries is **stepping over a thin surface**: a surface thinner than one increment in the direction
// the ray sweeps can fall between two samples and be skipped, dropping it out of the reflection — so
// a consumer seeing a thin object drop out raises the budget, which is why the budget is named and
// this cost documented rather than hidden.
//
// **A sky-sentinel sample keeps marching.** A step whose recorded distance is CaptureDistanceSky
// found no geometry in that direction: the comparison below never brackets there, because travelled
// never reaches the sentinel, so the march continues past it. At a silhouette this reports whatever
// the ray reaches beyond the edge rather than stopping at the near object's rim — the choice this
// module makes and states, because "stop and report a miss" would behave differently exactly there.

#include "Veng/octahedral.slang"
#include "Veng/capture_distance.slang"

// Default linear-search step budget when the caller names none (see the march contract above).
static const int CaptureDepthMarchSteps = 64;

// Binary-refine iterations after the linear search brackets the crossing. Each halves the residual,
// so this fixes the sub-step precision independent of the linear budget.
static const int CaptureDepthRefineSteps = 8;

// The finite far reach the search uses when the ray's own direction records only sky, in world
// units. A ray looking at open sky has no recorded distance to scale the search by, so it is marched
// to this bound and then misses — the sky answer. It cannot be the sentinel itself: the sentinel
// squared in a length() overflows the float range, which would bracket on an infinity. This is the
// module's one scene-scale assumption; a capture whose geometry sits beyond it raises it. Its square
// stays well inside the float range.
static const float CaptureDepthSkyReach = 1.0e9;

// The recorded radial distance in a direction (need not be unit), or CaptureDistanceSky where the
// capture saw no geometry. A near-zero direction — a march point at the probe centre — has no
// meaningful octahedral texel, so it reads as sky and brackets nothing.
float CaptureDistanceSample(Texture2D distanceMap, SamplerState pointSampler, float3 dir)
{
    if (dot(dir, dir) < 1e-16)
    {
        return CaptureDistanceSky;
    }
    return distanceMap.SampleLevel(pointSampler, OctahedralUV(dir), 0.0).r;
}

// Marches `rayDir` from `rayOrigin` against `distanceMap` over `steps` linear increments, returning
// the probe-centre-relative direction (UNNORMALIZED) of the surface crossed, or the bare `rayDir` on
// a miss. `steps` is the linear-search budget; the refine count is fixed (CaptureDepthRefineSteps).
float3 CaptureDepthCorrect(float3 rayOrigin, float3 rayDir, Texture2D distanceMap,
                           SamplerState pointSampler, int steps)
{
    const float3 dir = normalize(rayDir);

    // Far bound of the linear search, in ray units. The distance recorded along the ray from the
    // probe centre is the scene's own scale in this direction; doubling it covers a ray that starts
    // off-centre. A sky direction has no recorded distance, so the search runs to CaptureDepthSkyReach
    // and finds nothing — the defined sky miss.
    const float alongRay = CaptureDistanceSample(distanceMap, pointSampler, dir);
    const float tMax = CaptureDistanceIsSky(alongRay) ? CaptureDepthSkyReach : alongRay * 2.0;
    const float dt = tMax / float(max(steps, 1));

    float tPrev = 0.0;
    for (int i = 1; i <= steps; ++i)
    {
        const float t = dt * float(i);
        const float3 p = rayOrigin + dir * t;
        const float recorded = CaptureDistanceSample(distanceMap, pointSampler, p);
        // Crossing: distance travelled from the centre has reached the recorded surface distance in
        // p's own direction. A sky sample's recorded is CaptureDistanceSky, which travelled never
        // reaches, so a sky step never brackets and the march simply continues past it.
        if (length(p) >= recorded)
        {
            float lo = tPrev;
            float hi = t;
            for (int j = 0; j < CaptureDepthRefineSteps; ++j)
            {
                const float mid = 0.5 * (lo + hi);
                const float3 pm = rayOrigin + dir * mid;
                if (length(pm) >= CaptureDistanceSample(distanceMap, pointSampler, pm))
                {
                    hi = mid;
                }
                else
                {
                    lo = mid;
                }
            }
            return rayOrigin + dir * hi;
        }
        tPrev = t;
    }

    // No crossing within the budget: the ray is looking at the sky (see the contract above).
    return rayDir;
}

// The march at the default step budget (CaptureDepthMarchSteps).
float3 CaptureDepthCorrect(float3 rayOrigin, float3 rayDir, Texture2D distanceMap,
                           SamplerState pointSampler)
{
    return CaptureDepthCorrect(rayOrigin, rayDir, distanceMap, pointSampler, CaptureDepthMarchSteps);
}
