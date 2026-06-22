#include <Veng/Renderer/ShadowCascades.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

#include <Veng/Assert.h>
#include <Veng/Math/Frustum.h>

namespace Veng::Renderer
{
    namespace
    {
        // The fixed pull-back along the light axis when the scene bound is empty:
        // the cascade's near plane sits this far toward the light from the slice's
        // own near, so casters between the light and the slice still reach it.
        constexpr f32 EmptyBoundsPullback = 50.0f;

        // The eight world-space corners of a frustum slice, interpolated from the
        // full frustum's near→far corner pairs by the slice's split fractions. The
        // full frustum corners come from the inverse view-proj over the NDC cube
        // (Vulkan z range [0, 1]); the near/far pair share x,y so the linear-in-
        // view-depth interpolation is exact.
        std::array<vec3, 8> SliceCorners(const mat4& invViewProj, f32 nearFraction, f32 farFraction)
        {
            // The four near and four far corners of the full frustum, NDC z in
            // [0, 1] for Vulkan clip space.
            std::array<vec3, 4> frustumNear{};
            std::array<vec3, 4> frustumFar{};
            const std::array<vec2, 4> ndcXY = {vec2(-1.0f, -1.0f), vec2(1.0f, -1.0f),
                                               vec2(1.0f, 1.0f), vec2(-1.0f, 1.0f)};

            for (usize i = 0; i < 4; ++i)
            {
                const vec4 nearH = invViewProj * vec4(ndcXY[i].x, ndcXY[i].y, 0.0f, 1.0f);
                const vec4 farH = invViewProj * vec4(ndcXY[i].x, ndcXY[i].y, 1.0f, 1.0f);
                frustumNear[i] = vec3(nearH) / nearH.w;
                frustumFar[i] = vec3(farH) / farH.w;
            }

            std::array<vec3, 8> corners{};
            for (usize i = 0; i < 4; ++i)
            {
                const vec3 ray = frustumFar[i] - frustumNear[i];
                corners[i] = frustumNear[i] + ray * nearFraction;
                corners[i + 4] = frustumNear[i] + ray * farFraction;
            }
            return corners;
        }

        // A stable up vector for the light's lookAt basis: world up unless the
        // light points nearly straight up or down, in which case +Z, so the basis
        // never degenerates for a straight-down sun.
        vec3 StableUp(vec3 dir)
        {
            return std::abs(dir.y) > 0.99f ? vec3(0.0f, 0.0f, 1.0f) : vec3(0.0f, 1.0f, 0.0f);
        }

        // Clips a segment a→b against a set of inward half-spaces (a point p is inside
        // when dot(plane.xyz, p) + plane.w >= 0, the Frustum/AABB convention). On a
        // surviving segment, writes the inside sub-segment endpoints and returns true;
        // returns false when the segment lies wholly outside.
        bool ClipSegment(vec3 a, vec3 b, std::span<const vec4> planes, vec3& outA, vec3& outB)
        {
            const vec3 dir = b - a;
            f32 t0 = 0.0f;
            f32 t1 = 1.0f;
            for (const vec4& plane : planes)
            {
                const vec3 normal = vec3(plane);
                const f32 denom = glm::dot(normal, dir);
                const f32 dist = glm::dot(normal, a) + plane.w; // signed distance of a
                if (std::abs(denom) < 1e-12f)
                {
                    if (dist < 0.0f)
                    {
                        return false; // parallel to the plane and outside it
                    }
                    continue;
                }
                const f32 t = -dist / denom;
                if (denom > 0.0f)
                {
                    t0 = std::max(t0, t); // crossing into the half-space
                }
                else
                {
                    t1 = std::min(t1, t); // crossing out of the half-space
                }
                if (t0 > t1)
                {
                    return false;
                }
            }
            outA = a + dir * t0;
            outB = a + dir * t1;
            return true;
        }

        // The [min, max] view-space depth (-z) of the intersection of the camera frustum
        // and the scene AABB, or nullopt when they do not intersect. The intersection of
        // two convex polyhedra has its extreme points at a vertex of one inside the other
        // or an edge of one crossing a face of the other; clipping each frustum edge to
        // the AABB and each AABB edge to the frustum yields exactly those points, so their
        // view depths bracket the intersection's depth extent.
        std::optional<vec2> FrustumSceneDepthRange(const mat4& invViewProj, const mat4& viewProj,
                                                   const mat4& view, const AABB& bounds)
        {
            const std::array<vec3, 8> frustumCorners = SliceCorners(invViewProj, 0.0f, 1.0f);
            // Frustum edges: the near quad (0-3), the far quad (4-7), and the four
            // connectors. SliceCorners orders each quad around the ndcXY ring.
            static constexpr std::array<std::pair<int, int>, 12> frustumEdges = {{{0, 1},
                                                                                  {1, 2},
                                                                                  {2, 3},
                                                                                  {3, 0},
                                                                                  {4, 5},
                                                                                  {5, 6},
                                                                                  {6, 7},
                                                                                  {7, 4},
                                                                                  {0, 4},
                                                                                  {1, 5},
                                                                                  {2, 6},
                                                                                  {3, 7}}};

            // AABB corners indexed by axis bits (x<<2 | y<<1 | z); edges join corners
            // differing in exactly one bit.
            std::array<vec3, 8> aabbCorners{};
            for (int i = 0; i < 8; ++i)
            {
                aabbCorners[i] = vec3((i & 4) != 0 ? bounds.Max.x : bounds.Min.x,
                                      (i & 2) != 0 ? bounds.Max.y : bounds.Min.y,
                                      (i & 1) != 0 ? bounds.Max.z : bounds.Min.z);
            }
            static constexpr std::array<std::pair<int, int>, 12> aabbEdges = {{{0, 1},
                                                                               {2, 3},
                                                                               {4, 5},
                                                                               {6, 7},
                                                                               {0, 2},
                                                                               {1, 3},
                                                                               {4, 6},
                                                                               {5, 7},
                                                                               {0, 4},
                                                                               {1, 5},
                                                                               {2, 6},
                                                                               {3, 7}}};

            const std::array<vec4, 6> frustumPlanes = Frustum::FromViewProjection(viewProj).Planes;
            const std::array<vec4, 6> aabbPlanes = {
                vec4(1.0f, 0.0f, 0.0f, -bounds.Min.x), vec4(-1.0f, 0.0f, 0.0f, bounds.Max.x),
                vec4(0.0f, 1.0f, 0.0f, -bounds.Min.y), vec4(0.0f, -1.0f, 0.0f, bounds.Max.y),
                vec4(0.0f, 0.0f, 1.0f, -bounds.Min.z), vec4(0.0f, 0.0f, -1.0f, bounds.Max.z)};

            f32 depthMin = std::numeric_limits<f32>::max();
            f32 depthMax = std::numeric_limits<f32>::lowest();
            bool any = false;
            const auto consider = [&](vec3 point)
            {
                const f32 depth = -(view * vec4(point, 1.0f)).z; // view looks down -Z
                depthMin = std::min(depthMin, depth);
                depthMax = std::max(depthMax, depth);
                any = true;
            };

            vec3 clipA;
            vec3 clipB;
            for (const auto& [i, j] : frustumEdges)
            {
                if (ClipSegment(frustumCorners[i], frustumCorners[j], aabbPlanes, clipA, clipB))
                {
                    consider(clipA);
                    consider(clipB);
                }
            }
            for (const auto& [i, j] : aabbEdges)
            {
                if (ClipSegment(aabbCorners[i], aabbCorners[j], frustumPlanes, clipA, clipB))
                {
                    consider(clipA);
                    consider(clipB);
                }
            }

            if (!any)
            {
                return std::nullopt;
            }
            return vec2(depthMin, depthMax);
        }
    }

    CascadeData ComputeCascades(const CameraView& camera, vec3 lightDir, const AABB& sceneBounds,
                                const CascadeSettings& settings)
    {
        const mat4 view = camera.View();
        const mat4 viewProj = camera.ViewProjection();
        const mat4 invViewProj = glm::inverse(viewProj);

        f32 near = camera.GetNear();
        f32 far = camera.GetFar();

        // Fit the split range to the *visible* scene: the view-depth extent of the
        // intersection of the camera frustum and the scene bound, not the whole bound.
        // The camera's own far plane may sit far past the scene (an editor fly camera
        // defaults to a 1000-unit far), and a large scene the camera only partly sees
        // (zoomed into one corner, or a long view down a corridor) would otherwise spread
        // its cascades across the scene's full depth. Fitting to the visible slab packs
        // every cascade onto on-screen receivers, lifting texel density where it shows. A
        // bound the frustum misses keeps the camera's own range.
        if (!sceneBounds.IsEmpty())
        {
            const std::optional<vec2> visibleDepth =
                FrustumSceneDepthRange(invViewProj, viewProj, view, sceneBounds);
            if (visibleDepth.has_value())
            {
                const f32 fitNear = std::clamp(visibleDepth->x, camera.GetNear(), camera.GetFar());
                const f32 fitFar = std::clamp(visibleDepth->y, camera.GetNear(), camera.GetFar());
                // Adopt the tighter range only when it is non-degenerate.
                if (fitFar > fitNear)
                {
                    near = fitNear;
                    far = fitFar;
                }
            }
        }

        VE_ASSERT(far > near && near > 0.0f,
                  "ComputeCascades needs far > near > 0 (got near={}, far={})", near, far);

        CascadeData data{};
        data.Count = std::clamp(settings.Count, 1u, MaxCascades);

        const vec3 dir = glm::normalize(lightDir);
        const vec3 up = StableUp(dir);

        // PSSM split distances: blend logarithmic and uniform splits by Lambda.
        // d[0] is the near plane; d[Count] is the far plane. Cascade k spans
        // [d[k], d[k+1]] and SplitFar[k] = d[k+1].
        std::array<f32, MaxCascades + 1> splits{};
        splits[0] = near;
        for (u32 i = 1; i <= data.Count; ++i)
        {
            const f32 fraction = static_cast<f32>(i) / static_cast<f32>(data.Count);
            const f32 logSplit = near * std::pow(far / near, fraction);
            const f32 uniformSplit = near + (far - near) * fraction;
            splits[i] = settings.Lambda * logSplit + (1.0f - settings.Lambda) * uniformSplit;
        }
        // Pin the final split to the far plane exactly (float pow drifts).
        splits[data.Count] = far;

        for (u32 k = 0; k < data.Count; ++k)
        {
            data.SplitFar[k] = splits[k + 1];

            const f32 nearFraction = (splits[k] - near) / (far - near);
            const f32 farFraction = (splits[k + 1] - near) / (far - near);
            const std::array<vec3, 8> corners =
                SliceCorners(invViewProj, nearFraction, farFraction);

            // Bounding sphere of the slice corners: center is the centroid, radius
            // the max distance to it. A sphere is rotation-invariant, so the
            // cascade extent does not change as the camera turns (no size shimmer).
            vec3 center(0.0f);
            for (const vec3& corner : corners)
            {
                center += corner;
            }
            center /= 8.0f;

            f32 radius = 0.0f;
            for (const vec3& corner : corners)
            {
                radius = std::max(radius, glm::length(corner - center));
            }

            // Look at the sphere center from a point pulled back by radius along the light axis.
            const mat4 lightView = glm::lookAt(center - dir * radius, center, up);

            // Texel snapping: quantize the light-space box min to texel increments
            // so the box translates in texel steps, not sub-texel (no shimmer as
            // the camera moves). worldUnitsPerTexel is guarded against a degenerate
            // near-zero slice.
            const f32 worldUnitsPerTexel =
                radius > 1e-6f ? (2.0f * radius) / static_cast<f32>(settings.Resolution) : 1.0f;

            // The sphere center in light space: its X/Y are the box center, its Z
            // the box's depth midpoint (light view looks down -Z, so the eye at
            // +radius pull-back puts the center at z = -radius).
            const vec4 centerLight = lightView * vec4(center, 1.0f);
            f32 left =
                std::floor((centerLight.x - radius) / worldUnitsPerTexel) * worldUnitsPerTexel;
            f32 bottom =
                std::floor((centerLight.y - radius) / worldUnitsPerTexel) * worldUnitsPerTexel;
            f32 right = left + 2.0f * radius;
            f32 top = bottom + 2.0f * radius;

            // One-texel guard band on each side so a caster exactly on the slice
            // edge does not sample off its tile.
            left -= worldUnitsPerTexel;
            bottom -= worldUnitsPerTexel;
            right += worldUnitsPerTexel;
            top += worldUnitsPerTexel;

            // The slice's own depth extent along the light axis in light space.
            // glm::orthoZO takes positive distances in front of the eye; light view
            // looks down -Z, so a light-space z of -d is a front distance of d. The
            // deeper edge (more-negative z) is the larger front distance.
            f32 nearDistance = -(centerLight.z + radius);      // front distance to the near edge
            const f32 farDistance = -(centerLight.z - radius); // front distance to the far edge

            // Extend the near plane toward the light via the scene bound so casters
            // between the light and the slice still cast into it. Only the
            // light-axis near plane moves; the texel-snapped left/right/bottom/top
            // are untouched, so XY texel density is undisturbed.
            if (!sceneBounds.IsEmpty())
            {
                const AABB boundsLight = sceneBounds.Transformed(lightView);
                // The bound's nearest extent toward the light is its smallest front
                // distance, i.e. its largest (least-negative) light-space z.
                const f32 boundsNearDistance = -boundsLight.Max.z;
                nearDistance = std::min(nearDistance, boundsNearDistance);
            }
            else
            {
                nearDistance -= EmptyBoundsPullback;
            }

            mat4 ortho = glm::orthoZO(left, right, bottom, top, nearDistance, farDistance);
            ortho[1][1] *= -1.0f; // Vulkan clip space has Y pointing down.

            data.ViewProj[k] = ortho * lightView;
        }

        return data;
    }

    mat4 ComposeTileRemap(const mat4& cascadeViewProj, const u32 cascade, const u32 columns,
                          const u32 rows)
    {
        const f32 sx = 1.0f / static_cast<f32>(columns);
        const f32 sy = 1.0f / static_cast<f32>(rows);
        const f32 col = static_cast<f32>(cascade % columns);
        const f32 row = static_cast<f32>(cascade / columns);

        // NDC.xy in [-1,1] → atlas UV in [0,1] → the tile's window, then back to
        // the [-1,1] clip the sample's NDC.xy * 0.5 + 0.5 will undo. Z is left
        // unchanged (the depth compare is per-tile-agnostic). Built column-major
        // to match glm.
        mat4 remap(1.0f);
        // Scale NDC x,y by tile fraction.
        remap[0][0] = sx;
        remap[1][1] = sy;
        // Translate into the tile: map x ∈ [-1,1] within the full atlas. The
        // tile's NDC center is offset so the [0,1] UV lands in the tile window.
        remap[3][0] = sx * (2.0f * col + 1.0f) - 1.0f;
        remap[3][1] = sy * (2.0f * row + 1.0f) - 1.0f;
        return remap * cascadeViewProj;
    }
}
