#include <Veng/Renderer/ShadowCascades.h>

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include <Veng/Assert.h>

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
    }

    CascadeData ComputeCascades(const Camera& camera, vec3 lightDir, const AABB& sceneBounds,
                                const CascadeSettings& settings)
    {
        const f32 near = camera.GetNear();
        const f32 far = camera.GetFar();
        VE_ASSERT(far > near && near > 0.0f,
                  "ComputeCascades needs far > near > 0 (got near={}, far={})", near, far);

        CascadeData data{};
        data.Count = std::clamp(settings.Count, 1u, MaxCascades);

        const vec3 dir = glm::normalize(lightDir);
        const vec3 up = StableUp(dir);
        const mat4 invViewProj = glm::inverse(camera.ViewProjection());

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
}
