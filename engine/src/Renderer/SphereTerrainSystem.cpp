#include "SphereTerrainSystem.h"

#include <algorithm>
#include <cmath>

#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Math/Frustum.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include "SceneFloatingOrigin.h"
#include "SphereTerrainResidency.h"
#include "SphereTerrainSelection.h"

namespace Veng::Renderer
{
    namespace
    {
        u64 EntityValue(const Entity entity)
        {
            return (static_cast<u64>(entity.Generation) << 32) | entity.Index;
        }

        void Accumulate(SphereTerrainSelectionStats& total, const SphereTerrainSelectionStats& add)
        {
            total.DrawnTiles += add.DrawnTiles;
            for (usize level = 0; level < total.DrawnPerLevel.size(); level++)
            {
                total.DrawnPerLevel[level] += add.DrawnPerLevel[level];
            }
            total.WaitingSplits += add.WaitingSplits;
            total.HeldForBalance += add.HeldForBalance;
            total.SplitForBalance += add.SplitForBalance;
            total.Requests += add.Requests;
            total.Visited += add.Visited;
        }
    }

    i32 SphereTerrainRequestPriority(const bool split, const bool inFrustum, const f32 urgency)
    {
        // Splits the view waits on first, those in view ahead of those behind, each ordered by how
        // far its parent's error exceeds the threshold; prefetch last.
        if (!split)
        {
            return inFrustum ? 1000 : 0;
        }
        const i32 scaled = static_cast<i32>(std::clamp(urgency, 0.0f, 100.0f) * 1000.0f);
        return (inFrustum ? 400000 : 200000) + scaled;
    }

    void SphereTerrainSystem::Execute(const Scene& scene, const CameraView& camera,
                                      const f32 heightPixels, const u64 frame)
    {
        VE_PROFILE_SCOPE("Render/SphereTerrainSelect");
        m_Selections.clear();
        m_Totals = {};

        const FloatingOrigin origin = SceneFloatingOrigin(scene);
        const dvec3 cameraScene = dvec3(camera.GetPosition());
        const dvec3 eyeStable = origin.Position + cameraScene;
        const mat4 projection = camera.Projection();
        const f64 pixelsPerRadian = 0.5 * heightPixels * std::abs(projection[1][1]);
        const Frustum frustum = Frustum::FromViewProjection(camera.ViewProjection());

        for (auto [entity, terrain] : scene.View<SphereTerrain>())
        {
            if (!terrain.Visible || terrain.Source == nullptr)
            {
                continue;
            }
            SphereTerrainSource& source = *terrain.Source;
            const glm::dquat toBody = glm::inverse(terrain.Orientation);

            // Scene-space planes rewritten about the eye in the body-fixed frame:
            // n.(cam + R q) + w = (R^T n).q + (n.cam + w).
            std::array<glm::dvec4, 6> planes{};
            for (usize i = 0; i < planes.size(); i++)
            {
                const dvec3 normal = dvec3(vec3(frustum.Planes[i]));
                planes[i] = glm::dvec4(toBody * normal,
                                       glm::dot(normal, cameraScene) + frustum.Planes[i].w);
            }

            SphereTerrainSelectionInput input{
                .Radius = source.GetRadius(),
                .MinHeight = source.GetMinHeight(),
                .MaxHeight = source.GetMaxHeight(),
                .MaxLevel = source.GetMaxLevel(),
                .Eye = toBody * (eyeStable - terrain.Center),
                .PixelsPerRadian = pixelsPerRadian,
                .TargetPixelsPerQuad = std::max(terrain.TargetPixelsPerQuad, 1.0f),
                .Frustum = &planes,
                .Tiles = &source.GetResidency(),
            };
            for (u32 level = 0; level <= CubeSphere::MaxLevel; level++)
            {
                input.LevelHeightBounds[level] = source.GetGenerator().GetLevelHeightBound(level);
            }

            Memory& memory = m_Memory[source.GetSerial()];
            memory.LastFrame = frame;
            input.PreviousSplits = &memory.Splits;

            SphereTerrainSelectionResult result = SelectSphereTerrain(input);
            for (const CubeSphereTileKey key : result.Wanted)
            {
                source.Want(key);
            }
            for (const SphereTerrainSelectionRequest& request : result.Requests)
            {
                source.Request(request.Key, SphereTerrainRequestPriority(
                                                request.Kind == SphereTerrainRequestKind::Split,
                                                request.InFrustum, request.Urgency));
            }
            memory.Splits = std::move(result.Splits);
            Accumulate(m_Totals, result.Stats);
            m_Selections[EntityValue(entity)] = {.Drawn = std::move(result.Drawn),
                                                 .Stats = result.Stats};
        }

        std::erase_if(m_Memory, [frame](const auto& entry)
                      { return entry.second.LastFrame + MemoryFrames < frame; });

        VE_PROFILE_COUNTER("Render/SphereTerrainDrawn", static_cast<f64>(m_Totals.DrawnTiles));
        VE_PROFILE_COUNTER("Render/SphereTerrainWaitingSplits",
                           static_cast<f64>(m_Totals.WaitingSplits));
    }

    const vector<SphereTerrainDrawnTile>*
    SphereTerrainSystem::FindSelection(const Entity entity) const
    {
        const auto it = m_Selections.find(EntityValue(entity));
        return it != m_Selections.end() ? &it->second.Drawn : nullptr;
    }

    const SphereTerrainSelectionStats* SphereTerrainSystem::FindStats(const Entity entity) const
    {
        const auto it = m_Selections.find(EntityValue(entity));
        return it != m_Selections.end() ? &it->second.Stats : nullptr;
    }
}
