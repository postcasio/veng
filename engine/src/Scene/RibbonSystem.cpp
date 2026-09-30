#include <Veng/Scene/RibbonSystem.h>

#include <glm/geometric.hpp>

#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

namespace Veng
{
    void AdvanceTrail(Trail& trail, const vec3& head, const f32 delta)
    {
        for (TrailSample& sample : trail.Samples)
        {
            sample.Age += delta;
        }
        std::erase_if(trail.Samples, [lifetime = trail.Lifetime](const TrailSample& sample)
                      { return sample.Age >= lifetime; });

        if (trail.Emitting &&
            (trail.Samples.empty() ||
             glm::distance(head, trail.Samples.back().Position) > trail.MinSampleDistance))
        {
            trail.Samples.push_back(TrailSample{.Position = head, .Age = 0.0f});
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
            const vec3 head(scene.GetInterpolatedWorldTransform(entity, context.Alpha)[3]);
            AdvanceTrail(trail, head, delta);
        }
    }
}
