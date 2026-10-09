#include <Veng/Scene/RibbonSystem.h>

#include <glm/geometric.hpp>

#include <cmath>

#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

namespace Veng
{
    void AdvanceTrail(Trail& trail, const vec3& head, const f32 delta)
    {
        mat4 pose(1.0f);
        pose[3] = vec4(head, 1.0f);
        AdvanceTrail(trail, pose, delta);
    }

    f32 TrailCrossSectionWidth(const vec3& semiX, const vec3& semiY, const vec3& tangent,
                               const vec3& eyeToPoint)
    {
        vec3 side = glm::cross(tangent, eyeToPoint);
        if (glm::dot(side, side) < 1e-12f)
        {
            side = glm::cross(tangent, std::abs(tangent.y) < 0.9f ? vec3(0.0f, 1.0f, 0.0f)
                                                                  : vec3(1.0f, 0.0f, 0.0f));
        }
        side = glm::normalize(side);
        const f32 x = glm::dot(semiX, side);
        const f32 y = glm::dot(semiY, side);
        return 2.0f * std::sqrt((x * x) + (y * y));
    }

    void AdvanceTrail(Trail& trail, const mat4& head, const f32 delta)
    {
        const vec3 position(head[3]);
        for (TrailSample& sample : trail.Samples)
        {
            sample.Age += delta;
        }
        std::erase_if(trail.Samples, [lifetime = trail.Lifetime](const TrailSample& sample)
                      { return sample.Age >= lifetime; });

        if (trail.Emitting &&
            (trail.Samples.empty() ||
             glm::distance(position, trail.Samples.back().Position) > trail.MinSampleDistance))
        {
            trail.Samples.push_back(TrailSample{
                .Position = position, .Age = 0.0f, .AxisX = vec3(head[0]), .AxisY = vec3(head[1])});
        }

        if (trail.Samples.size() > trail.MaxSamples)
        {
            trail.Samples.erase(
                trail.Samples.begin(),
                trail.Samples.begin() +
                    static_cast<std::ptrdiff_t>(trail.Samples.size() - trail.MaxSamples));
        }
    }

    Trail& AttachTrail(Scene& scene, const Entity entity, const Trail& trail)
    {
        Trail fresh = trail;
        fresh.Samples.clear();
        if (auto* existing = scene.TryGet<Trail>(entity))
        {
            *existing = std::move(fresh);
            return *existing;
        }
        return scene.Add<Trail>(entity, std::move(fresh));
    }

    void OffsetRibbons(Scene& scene, const vec3& offset)
    {
        for (auto [entity, ribbon] : scene.View<Ribbon>())
        {
            ribbon.From += offset;
            ribbon.To += offset;
        }
        for (auto [entity, trail] : scene.View<Trail>())
        {
            for (TrailSample& sample : trail.Samples)
            {
                sample.Position += offset;
            }
        }
    }

    void RibbonSystem::OnUpdate(Scene& scene, const f32 delta, const SystemContext& context)
    {
        for (auto [entity, ribbon] : scene.View<Ribbon>())
        {
            ribbon.Age += delta;
        }

        for (auto [entity, trail] : scene.View<Trail>())
        {
            if (!scene.Has<Transform>(entity))
            {
                continue;
            }
            AdvanceTrail(trail, scene.GetInterpolatedWorldTransform(entity, context.Alpha), delta);
        }
    }
}
