#include "InstancedCull.h"

#include <algorithm>

namespace Veng::Renderer
{
    InstancedCullVerdict CullInstance(const MeshInstance& instance, const InstancedCullSet& set,
                                      const InstancedCullView& view,
                                      const InstancedCullReference& reference, const u8 previous)
    {
        const mat4 model = set.World * instance.GetMatrix();
        vec3 center;
        f32 radius = 0.0f;
        InstanceBoundingSphere(model, set.MeshBounds, center, radius);

        if (view.FrustumCull && !Intersects(view.Planes, set.MeshBounds.Transformed(model)))
        {
            return {};
        }

        // The band is measured in the main camera in every view, so a shadow view casts exactly
        // the instances the main view's band keeps.
        const f32 pixels = ReferenceProjectedPixels(reference.ViewProj, reference.Eye,
                                                    reference.HeightPixels, center, radius);
        if (pixels < set.MinPixels || pixels > set.MaxPixels)
        {
            return {};
        }

        if (!view.Main && set.MinShadowTexels > 0.0f)
        {
            LodView unbiased = view.Lod;
            unbiased.ScreenScale = 1.0f;
            if (ProjectedScreenSize(unbiased, center, radius) * view.Resolution <
                set.MinShadowTexels)
            {
                return {};
            }
        }

        if (set.LodCount <= 1)
        {
            return {.Drawn = true, .Lod = 0};
        }
        const i32 forced = set.ForcedLod >= 0 ? set.ForcedLod : view.ForcedLod;
        if (forced >= 0)
        {
            return {.Drawn = true, .Lod = std::min(static_cast<u32>(forced), set.LodCount - 1)};
        }
        const u32 last = previous == InstanceNoPreviousLod
                             ? NoPreviousLod
                             : std::min(static_cast<u32>(previous), set.LodCount - 1);
        const u32 level =
            SelectLod(set.LodThresholds, ProjectedScreenSize(view.Lod, center, radius), last,
                      view.Hysteresis);
        return {.Drawn = true, .Lod = std::min(level, set.LodCount - 1)};
    }
}
