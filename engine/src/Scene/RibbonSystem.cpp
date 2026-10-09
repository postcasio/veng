#include <Veng/Scene/RibbonSystem.h>

#include <glm/geometric.hpp>

#include <cmath>

#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

namespace Veng
{
    namespace
    {
        // A local velocity carried into world space by the head's rotation alone, so the emitter's
        // scale shapes its cross-section and not its exhaust speed.
        vec3 EmitDirection(const mat4& head, const vec3& local)
        {
            vec3 world(0.0f);
            for (int axis = 0; axis < 3; ++axis)
            {
                const vec3 column(head[axis]);
                const f32 length = glm::length(column);
                if (length > 0.0f)
                {
                    world += column * (local[axis] / length);
                }
            }
            return world;
        }

        // Drops every sample a newer one has overtaken — faster exhaust sweeping up slower exhaust
        // ahead of it — so the chain, which joins samples in emission order, never doubles back.
        // A kept sample is dropped when the step from it to the next point (a newer sample, or the
        // head) turns back on the step into it.
        void SweepOvertaken(vector<TrailSample>& samples, const vec3& head)
        {
            usize kept = 0;
            const auto doublesBack = [&samples, &kept](const vec3& next)
            {
                if (kept < 2)
                {
                    return false;
                }
                const vec3 into = samples[kept - 1].Position - samples[kept - 2].Position;
                const vec3 onward = next - samples[kept - 1].Position;
                return glm::dot(into, onward) < 0.0f;
            };
            for (usize i = 0; i < samples.size(); ++i)
            {
                while (doublesBack(samples[i].Position))
                {
                    --kept;
                }
                samples[kept++] = samples[i];
            }
            while (doublesBack(head))
            {
                --kept;
            }
            samples.resize(kept);
        }
    }

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
        vec3 emitter(0.0f);
        if (trail.HasEmitterVelocity)
        {
            emitter = trail.EmitterVelocity;
        }
        else if (trail.HasPreviousHead && delta > 0.0f)
        {
            emitter = (position - trail.PreviousHead) / delta;
        }
        trail.HasEmitterVelocity = false;
        trail.PreviousHead = position;
        trail.HasPreviousHead = true;

        // Exact under a constant drag, so a sample travels the same distance at any frame rate.
        const f32 keep = trail.Drag > 0.0f ? std::exp(-trail.Drag * delta) : 1.0f;
        const f32 travel = trail.Drag > 0.0f ? (1.0f - keep) / trail.Drag : delta;
        for (TrailSample& sample : trail.Samples)
        {
            sample.Position += sample.Velocity * travel;
            sample.Velocity *= keep;
            sample.Age += delta;
        }
        std::erase_if(trail.Samples, [lifetime = trail.Lifetime](const TrailSample& sample)
                      { return sample.Age >= lifetime; });
        if (trail.Drag > 0.0f || trail.InheritVelocity != 0.0f || trail.EmitVelocity != vec3(0.0f))
        {
            SweepOvertaken(trail.Samples, position);
        }

        if (trail.Emitting &&
            (trail.Samples.empty() ||
             glm::distance(position, trail.Samples.back().Position) > trail.MinSampleDistance))
        {
            trail.Samples.push_back(TrailSample{
                .Position = position,
                .Age = 0.0f,
                .AxisX = vec3(head[0]),
                .AxisY = vec3(head[1]),
                .Velocity =
                    (trail.InheritVelocity * emitter) + EmitDirection(head, trail.EmitVelocity),
            });
        }

        if (trail.Samples.size() > trail.MaxSamples)
        {
            trail.Samples.erase(
                trail.Samples.begin(),
                trail.Samples.begin() +
                    static_cast<std::ptrdiff_t>(trail.Samples.size() - trail.MaxSamples));
        }
    }

    void RestartTrail(Trail& trail)
    {
        trail.Samples.clear();
        trail.HasPreviousHead = false;
    }

    Trail& AttachTrail(Scene& scene, const Entity entity, const Trail& trail)
    {
        Trail fresh = trail;
        RestartTrail(fresh);
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
            trail.PreviousHead += offset;
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
