#include "LodSelection.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <Veng/Asset/Mesh.h>
#include <Veng/Scene/Visibility.h>

namespace Veng::Renderer
{
    namespace
    {
        // Below this a projection's w row is treated as absent: an orthographic projection.
        constexpr f32 PerspectiveEpsilon = 1e-6f;
    }

    LodView LodView::FromViewProjection(const mat4& viewProj, const f32 bias)
    {
        // glm is column-major: viewProj[c][r]. The y row's spatial part is the projection's
        // vertical scale times a unit row of the rigid view rotation.
        const vec3 yRow(viewProj[0][1], viewProj[1][1], viewProj[2][1]);
        const vec3 wRow(viewProj[0][3], viewProj[1][3], viewProj[2][3]);

        LodView view;
        view.ProjectionScale = glm::length(yRow);
        view.Perspective = glm::length(wRow) > PerspectiveEpsilon;
        view.ScreenScale = std::exp2(-bias);
        if (view.Perspective)
        {
            // The eye is the projection's centre: the point clip space maps to (0, 0, z, 0), which
            // is the inverse's z column.
            const vec4 eye = glm::inverse(viewProj)[2];
            view.Eye = std::abs(eye.w) > PerspectiveEpsilon ? vec3(eye) / eye.w : vec3(0.0f);
        }
        return view;
    }

    f32 ProjectedScreenSize(const LodView& view, const vec3& center, const f32 radius)
    {
        if (!view.Perspective)
        {
            return radius * view.ProjectionScale * view.ScreenScale;
        }
        const f32 distance = glm::length(center - view.Eye);
        if (distance <= radius)
        {
            return std::numeric_limits<f32>::max();
        }
        return radius * view.ProjectionScale / distance * view.ScreenScale;
    }

    u32 SelectLod(const std::span<const f32> thresholds, const f32 screenSize, const u32 previous,
                  const f32 hysteresis)
    {
        // The thresholds descend, so each count is the length of the prefix the size falls below:
        // `coarsest` past the bands' low edges, `finest` past their high edges.
        u32 coarsest = 0;
        u32 middle = 0;
        u32 finest = 0;
        for (const f32 threshold : thresholds)
        {
            if (screenSize >= threshold * (1.0f + hysteresis))
            {
                break;
            }
            ++finest;
            if (screenSize < threshold)
            {
                ++middle;
            }
            if (screenSize < threshold * (1.0f - hysteresis))
            {
                ++coarsest;
            }
        }
        if (previous == NoPreviousLod)
        {
            return middle;
        }
        return std::clamp(previous, coarsest, finest);
    }

    u32 ResolveMeshLod(const VisibleMesh& item, const LodView& view, const LodPolicy& policy,
                       const u32 previous)
    {
        const Mesh& mesh = *item.Mesh;
        const u32 count = mesh.GetLodCount();
        if (count == 1)
        {
            return 0;
        }

        const i32 forced = item.ForcedLod >= 0 ? item.ForcedLod : policy.Forced;
        if (forced >= 0)
        {
            return std::min(static_cast<u32>(forced), count - 1);
        }

        // The level-0 bound's sphere, carried to world space: its centre by the world matrix and its
        // radius by the largest axis scale, so the size is unchanged by the mesh turning.
        const AABB& bounds = mesh.GetBounds();
        const vec3 center = vec3(item.World * vec4(bounds.Center(), 1.0f));
        const f32 scale =
            std::max({glm::length(vec3(item.World[0])), glm::length(vec3(item.World[1])),
                      glm::length(vec3(item.World[2]))});
        const f32 radius = glm::length(bounds.Max - bounds.Min) * 0.5f * scale;

        const f32 screenSize = ProjectedScreenSize(view, center, radius);
        const u32 level =
            SelectLod(mesh.GetLodScreenSizes(), screenSize,
                      previous == NoPreviousLod ? previous : std::min(previous, count - 1),
                      policy.Hysteresis);
        return std::min(level, count - 1);
    }
}
