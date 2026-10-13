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

        // The view-space point at view depth `depth` (view looks down -Z) that projects to
        // `ndc`, solved from the projection's own coefficients for the skew-free perspective
        // and orthographic matrices a CameraView builds (clip w = P[3][3] − P[2][3]·depth).
        // Nothing is inverted and nothing is reconstructed through NDC depth, so it is exact at
        // any depth however small the render near — under reverse-Z a tiny near sends the far
        // plane to w = 0, where a reconstruction through the inverse view-proj is inf.
        vec3 ViewPointAtDepth(const mat4& proj, vec2 ndc, f32 depth)
        {
            const f32 w = proj[3][3] - proj[2][3] * depth;
            return {(ndc.x * w + proj[2][0] * depth - proj[3][0]) / proj[0][0],
                    (ndc.y * w + proj[2][1] * depth - proj[3][1]) / proj[1][1], -depth};
        }

        // The eight world-space corners of the frustum between two view-space depths:
        // corners[0..3] the nearDepth plane, [4..7] the farDepth plane, each quad ordered around
        // the ndcXY ring. Each corner is solved in view space and only then placed as
        // eye + rotation·offset, so the offset keeps full precision. Differencing two
        // world-space points instead — a near-plane corner against the eye — loses the whole
        // offset to rounding once a micron near's corners sit within one float ulp of an eye
        // away from the origin, turning the frustum's shape into noise.
        std::array<vec3, 8> FrustumCornersAtDepths(const mat4& proj, const mat3& viewToWorld,
                                                   vec3 eye, f32 nearDepth, f32 farDepth)
        {
            const std::array<vec2, 4> ndcXY = {vec2(-1.0f, -1.0f), vec2(1.0f, -1.0f),
                                               vec2(1.0f, 1.0f), vec2(-1.0f, 1.0f)};
            std::array<vec3, 8> corners{};
            for (usize i = 0; i < 4; ++i)
            {
                corners[i] = eye + viewToWorld * ViewPointAtDepth(proj, ndcXY[i], nearDepth);
                corners[i + 4] = eye + viewToWorld * ViewPointAtDepth(proj, ndcXY[i], farDepth);
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
        std::optional<vec2> FrustumSceneDepthRange(const std::array<vec3, 8>& frustumCorners,
                                                   const mat4& viewProj, const mat4& view,
                                                   const AABB& bounds)
        {
            // Frustum edges: the near quad (0-3), the far quad (4-7), and the four
            // connectors. FrustumCornersAtDepths orders each quad around the ndcXY ring.
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

    mat3 CascadeLightBasis(const vec3 lightDir)
    {
        const vec3 dir = glm::normalize(lightDir);
        const vec3 right = glm::normalize(glm::cross(dir, StableUp(dir)));
        const vec3 up = glm::cross(right, dir);
        return glm::transpose(mat3(right, up, -dir));
    }

    f32 CascadeStepRadius(const i32 step)
    {
        return std::exp2(static_cast<f32>(step) / static_cast<f32>(CascadeRadiusStepsPerOctave));
    }

    CascadeData ComputeCascades(const CameraView& camera, vec3 lightDir, const AABB& sceneBounds,
                                const CascadeSettings& settings, const CascadeFrame& frame)
    {
        VE_ASSERT(settings.Resolution > 4 && settings.Resolution % 2 == 0,
                  "ComputeCascades needs an even Resolution above 4 (got {})", settings.Resolution);

        const mat4 view = camera.View();
        const mat4 viewProj = camera.ViewProjection();
        const mat4 proj = camera.Projection();

        const f32 cameraNear = camera.GetNear();
        const f32 cameraFar = camera.GetFar();

        // Every frustum corner below is solved in view space at its depth and carried to world
        // by the inverse view, so neither the far plane — which reverse-Z sends to infinity when
        // the render near is tiny — nor the micron near plane is ever reconstructed.
        const mat4 invView = glm::inverse(view);
        const mat3 viewToWorld(invView);
        const vec3 eye(invView[3]);

        // Floor the shadow near independently of the render near (settings.MinDistance; 0
        // keeps the camera near). A tiny render near against a distant far degenerates both
        // the split distribution and the slice reconstruction; flooring the shadow near
        // bounds the fit the near-side way MaxDistance bounds the far.
        const f32 shadowNear = std::max(cameraNear, settings.MinDistance);

        f32 near = shadowNear;
        f32 far = cameraFar;

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
            const std::array<vec3, 8> frustumCorners =
                FrustumCornersAtDepths(proj, viewToWorld, eye, cameraNear, cameraFar);
            const std::optional<vec2> visibleDepth =
                FrustumSceneDepthRange(frustumCorners, viewProj, view, sceneBounds);
            if (visibleDepth.has_value())
            {
                const f32 fitNear = std::clamp(visibleDepth->x, shadowNear, cameraFar);
                const f32 fitFar = std::clamp(visibleDepth->y, shadowNear, cameraFar);
                // Adopt the tighter range only when it is non-degenerate.
                if (fitFar > fitNear)
                {
                    near = fitNear;
                    far = fitFar;
                }
            }
        }

        // Cap the shadowed range: a fitted far past MaxDistance spreads cascades over
        // world the shadows cannot usefully resolve. Skipped when the cap would not
        // leave a valid range (a camera already inside the cap distance of its near).
        if (settings.MaxDistance > 0.0f && settings.MaxDistance > near)
        {
            far = std::min(far, settings.MaxDistance);
        }

        VE_ASSERT(far > near && near > 0.0f,
                  "ComputeCascades needs far > near > 0 (got near={}, far={})", near, far);

        CascadeData data{};
        data.Count = std::clamp(settings.Count, 1u, MaxCascades);

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

        const mat3 basis = CascadeLightBasis(lightDir);

        // Where the scene origin stands in the stable frame, on the light's axes. The only
        // double-precision step: everything after it works with small render-space magnitudes.
        // glm is column-major, so basis[c][r] is row r's component c.
        std::array<f64, 2> originLight{};
        for (glm::length_t r = 0; r < 2; ++r)
        {
            originLight[r] = (static_cast<f64>(basis[0][r]) * frame.Origin.x) +
                             (static_cast<f64>(basis[1][r]) * frame.Origin.y) +
                             (static_cast<f64>(basis[2][r]) * frame.Origin.z);
        }

        const CascadeData* previous =
            frame.Previous != nullptr && frame.Previous->Count == data.Count ? frame.Previous
                                                                             : nullptr;

        const std::array<vec2, 4> ndcXY = {vec2(-1.0f, -1.0f), vec2(1.0f, -1.0f), vec2(1.0f, 1.0f),
                                           vec2(-1.0f, 1.0f)};
        const f32 resolution = static_cast<f32>(settings.Resolution);

        for (u32 k = 0; k < data.Count; ++k)
        {
            data.SplitFar[k] = splits[k + 1];

            // The slice's bounding sphere, measured in view space: the centroid of its eight
            // corners and the largest distance to it. The corners come from the projection and
            // the splits alone, so for given splits the radius does not depend on the camera's
            // orientation to the bit; only the centroid is carried to world space.
            std::array<vec3, 8> corners{};
            for (usize i = 0; i < 4; ++i)
            {
                corners[i] = ViewPointAtDepth(proj, ndcXY[i], splits[k]);
                corners[i + 4] = ViewPointAtDepth(proj, ndcXY[i], splits[k + 1]);
            }
            vec3 centroid(0.0f);
            for (const vec3& corner : corners)
            {
                centroid += corner;
            }
            centroid /= 8.0f;
            f32 radius = 0.0f;
            for (const vec3& corner : corners)
            {
                radius = std::max(radius, glm::length(corner - centroid));
            }
            radius = std::max(radius, 1e-6f);
            const vec3 center = eye + viewToWorld * centroid;

            // Round the radius up onto the ladder, holding last frame's step while the need sits
            // at most one step below it: the size grows at once and does not flip on a need that
            // hovers across a boundary.
            i32 step = static_cast<i32>(
                std::ceil(static_cast<f32>(CascadeRadiusStepsPerOctave) * std::log2(radius)));
            if (CascadeStepRadius(step) < radius)
            {
                ++step;
            }
            if (previous != nullptr)
            {
                const i32 held = previous->RadiusStep[k];
                if (step >= held - 1 && step <= held)
                {
                    step = held;
                }
            }
            data.RadiusStep[k] = step;

            // The pitch leaves two texels beside the sphere on each side of a Resolution-texel
            // box: half a texel for the snap's move and one for the guard band, with room over.
            const f32 texel = 2.0f * CascadeStepRadius(step) / (resolution - 4.0f);

            // A cascade whose slice begins inside the viewer's reach snaps in scene space, which
            // rides the viewer (zero phase); the rest snap on the stable frame's grid.
            const bool viewerFrame = splits[k] < frame.ViewerFrameReach;
            if (viewerFrame)
            {
                data.ViewerFrameCount = k + 1;
            }
            vec2 phase(0.0f);
            if (!viewerFrame)
            {
                const f64 pitch = texel;
                phase = vec2(
                    static_cast<f32>(originLight[0] - pitch * std::floor(originLight[0] / pitch)),
                    static_cast<f32>(originLight[1] - pitch * std::floor(originLight[1] / pitch)));
            }

            // Snap the sphere's light-space centre to the grid: s + phase is a whole multiple of
            // the pitch, so every texel line of the box sits on the frame's grid. The light view
            // places s at the origin and pulls back by the sphere radius along the light axis.
            const vec3 centerLight = basis * center;
            const vec2 snapped = texel * glm::round((vec2(centerLight) + phase) / texel) - phase;
            mat4 lightView(basis);
            lightView[3] = vec4(-snapped.x, -snapped.y, -centerLight.z - radius, 1.0f);

            // The box is exactly Resolution texels wide on both axes.
            const f32 half = 0.5f * resolution * texel;
            const f32 left = -half;
            const f32 right = half;
            const f32 bottom = -half;
            const f32 top = half;

            // The sphere's own depth extent along the light axis. glm::orthoZO takes positive
            // distances in front of the eye; the light view puts the centre a radius in front.
            const f32 tightNearDistance = 0.0f;
            const f32 farDistance = 2.0f * radius;

            // The near plane extended toward the light via the scene bound so casters
            // between the light and the slice still reach it. Only the light-axis near
            // plane moves; the texel-snapped left/right/bottom/top are untouched, so XY
            // texel density is undisturbed.
            f32 extendedNearDistance = tightNearDistance;
            if (!sceneBounds.IsEmpty())
            {
                const AABB boundsLight = sceneBounds.Transformed(lightView);
                // The bound's nearest extent toward the light is its smallest front
                // distance, i.e. its largest (least-negative) light-space z.
                const f32 boundsNearDistance = -boundsLight.Max.z;
                extendedNearDistance = std::min(extendedNearDistance, boundsNearDistance);
            }
            else
            {
                extendedNearDistance -= EmptyBoundsPullback;
            }

            // The render matrix keeps the tight near under PancakeNear (depth clamp
            // pancakes nearer casters onto it), so its NDC-z resolution tracks the slice,
            // not the scene's extent along the light. The cull matrix always spans the
            // extended range so those casters survive the broadphase cull.
            const f32 renderNearDistance =
                settings.PancakeNear ? tightNearDistance : extendedNearDistance;

            mat4 ortho = glm::orthoZO(left, right, bottom, top, renderNearDistance, farDistance);
            ortho[1][1] *= -1.0f; // Vulkan clip space has Y pointing down.
            // Reverse-Z: the shadow atlas follows the engine-wide near→1, far→0 convention.
            data.ViewProj[k] = ReverseZClip() * ortho * lightView;

            mat4 cullOrtho =
                glm::orthoZO(left, right, bottom, top, extendedNearDistance, farDistance);
            cullOrtho[1][1] *= -1.0f;
            data.CullViewProj[k] = ReverseZClip() * cullOrtho * lightView;

            data.TexelWorldSize[k] = texel;
            data.DepthRange[k] = farDistance - renderNearDistance;
        }

        return data;
    }

    mat4 ComposeTileRemap(const mat4& cascadeViewProj, const u32 cascade, const u32 set,
                          const ShadowAtlasGrid& grid)
    {
        const u32 totalRows = grid.TotalRows();
        const f32 sx = 1.0f / static_cast<f32>(grid.Columns);
        const f32 sy = 1.0f / static_cast<f32>(totalRows);
        const f32 col = static_cast<f32>(cascade % grid.Columns);
        // Sets stack as row bands: set s starts at row s · Rows.
        const f32 row = static_cast<f32>(set * grid.Rows + cascade / grid.Columns);

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
