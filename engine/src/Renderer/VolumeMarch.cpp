#include <Veng/Renderer/VolumeMarch.h>

#include <algorithm>

#include <glm/common.hpp>

namespace Veng::Renderer
{
    std::optional<MarchSegment> ComputeMarchSegment(const vec3 origin, const vec3 dir,
                                                    const AABB& bounds, const f32 maxDistance)
    {
        // Slab intersection: per axis, the two ray parameters where the ray crosses the min and max
        // planes. A zero dir component yields ±inf, which resolves correctly under IEEE min/max — a
        // ray parallel to a slab is unconstrained by it when inside, a miss when outside.
        const vec3 invDir = 1.0f / dir;
        const vec3 t0 = (bounds.Min - origin) * invDir;
        const vec3 t1 = (bounds.Max - origin) * invDir;
        const vec3 tSmall = glm::min(t0, t1);
        const vec3 tLarge = glm::max(t0, t1);

        f32 enter = std::max({tSmall.x, tSmall.y, tSmall.z});
        f32 exit = std::min({tLarge.x, tLarge.y, tLarge.z});

        // Clamp the entry to the camera (a camera inside the box marches from 0) and the exit to the
        // nearest opaque surface (geometry in front of the far face shortens the march).
        enter = std::max(enter, 0.0f);
        exit = std::min(exit, maxDistance);

        // A missed box (exit < enter before clamping), a grazing hit, or geometry in front of the
        // near face (maxDistance < enter) all collapse to an empty segment — nothing to march.
        if (exit <= enter)
        {
            return std::nullopt;
        }
        return MarchSegment{.Enter = enter, .Exit = exit};
    }

    bool VolumeFieldFartherFirst(const vec3 cameraPos, const AABB& a, const AABB& b)
    {
        const vec3 da = a.Center() - cameraPos;
        const vec3 db = b.Center() - cameraPos;
        return glm::dot(da, da) > glm::dot(db, db);
    }
}
