#include <Veng/Renderer/PunctualShadows.h>

#include <algorithm>
#include <cmath>
#include <numbers>

#include <glm/gtc/matrix_transform.hpp>

#include <Veng/Assert.h>

namespace Veng::Renderer
{
    namespace
    {
        // The shadow near plane as a fraction of the light's range, floored at a
        // fixed minimum so a tiny-range light still has a non-degenerate frustum.
        constexpr f32 NearFraction = 0.05f;
        constexpr f32 MinNear = 0.05f;

        // A stable up vector for the spot's lookAt basis: world up unless the light
        // points nearly straight up or down, in which case +Z, so the basis never
        // degenerates for a straight-down spot.
        vec3 StableUp(vec3 dir)
        {
            return std::abs(dir.y) > 0.99f ? vec3(0.0f, 0.0f, 1.0f) : vec3(0.0f, 1.0f, 0.0f);
        }

        f32 ShadowNear(f32 range)
        {
            return std::max(range * NearFraction, MinNear);
        }
    }

    SpotShadowView ComputeSpotShadowView(vec3 position, vec3 direction, f32 range, f32 outerCone)
    {
        constexpr f32 HalfPi = std::numbers::pi_v<f32> / 2.0f;
        VE_ASSERT(range > 0.0f, "ComputeSpotShadowView needs range > 0 (got {})", range);
        VE_ASSERT(outerCone > 0.0f && outerCone < HalfPi,
                  "ComputeSpotShadowView needs outerCone in (0, π/2) (got {})", outerCone);

        const f32 near = ShadowNear(range);
        const f32 far = range;

        // The cone's full angular width, so the shadow frustum exactly contains the
        // lit cone. Clamped below π so a wide cone never degenerates the projection.
        constexpr f32 Epsilon = 1e-3f;
        const f32 fovy = std::clamp(2.0f * outerCone, Epsilon, std::numbers::pi_v<f32> - Epsilon);

        const vec3 dir = glm::normalize(direction);
        const vec3 up = StableUp(dir);
        const mat4 view = glm::lookAt(position, position + dir, up);

        mat4 proj = glm::perspectiveZO(fovy, 1.0f, near, far);
        proj[1][1] *= -1.0f; // Vulkan clip space has Y pointing down.

        return {.ViewProj = proj * view, .Near = near, .Far = far};
    }

    PointShadowView ComputePointShadowView(vec3 position, f32 range)
    {
        VE_ASSERT(range > 0.0f, "ComputePointShadowView needs range > 0 (got {})", range);

        const f32 near = ShadowNear(range);
        const f32 far = range;

        // The canonical cube-face forward/up basis a Vulkan cube map expects, in
        // CubeFace order (+X, -X, +Y, -Y, +Z, -Z). The ±Y faces use a ±Z up by
        // construction, so the six fixed faces cover every direction with no
        // near-vertical degeneracy and need no stable-up swap.
        constexpr std::array<vec3, CubeFaceCount> Forwards = {
            vec3(1.0f, 0.0f, 0.0f), vec3(-1.0f, 0.0f, 0.0f),
            vec3(0.0f, 1.0f, 0.0f), vec3(0.0f, -1.0f, 0.0f),
            vec3(0.0f, 0.0f, 1.0f), vec3(0.0f, 0.0f, -1.0f)};
        constexpr std::array<vec3, CubeFaceCount> Ups = {
            vec3(0.0f, -1.0f, 0.0f), vec3(0.0f, -1.0f, 0.0f),
            vec3(0.0f, 0.0f, 1.0f), vec3(0.0f, 0.0f, -1.0f),
            vec3(0.0f, -1.0f, 0.0f), vec3(0.0f, -1.0f, 0.0f)};

        mat4 proj = glm::perspectiveZO(std::numbers::pi_v<f32> / 2.0f, 1.0f, near, far);
        proj[1][1] *= -1.0f; // Vulkan clip space has Y pointing down.

        PointShadowView result{};
        result.Near = near;
        result.Far = far;
        for (u32 f = 0; f < CubeFaceCount; ++f)
        {
            const mat4 view = glm::lookAt(position, position + Forwards[f], Ups[f]);
            result.ViewProj[f] = proj * view;
        }
        return result;
    }
}
