#include <Veng/Scene/ParticleEffectSystem.h>

#include <algorithm>

#include <glm/gtc/quaternion.hpp>

#include <Veng/Asset/ParticleEffect.h>
#include <Veng/Math/Random.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

namespace Veng
{
    void LatchParticleEffect(const Scene& scene, const Entity entity,
                             ParticleEffectInstance& instance)
    {
        const mat4 world = WorldMatrix(scene, entity);
        const auto* origin = scene.TryGetFirst<FloatingOrigin>();
        const dvec3 originPosition = origin != nullptr ? origin->Position : dvec3(0.0);
        instance.Anchor = dvec3(vec3(world[3])) + originPosition;

        const mat3 basis(glm::normalize(vec3(world[0])), glm::normalize(vec3(world[1])),
                         glm::normalize(vec3(world[2])));
        instance.Orientation = glm::normalize(glm::quat_cast(basis));
        if (instance.Seed == 0)
        {
            const u64 bits = (static_cast<u64>(entity.Generation) << 32) | entity.Index;
            instance.Seed = static_cast<u32>(Hash64(bits ^ instance.Playback));
        }
        instance.Anchored = true;
    }

    void PruneParticleFeed(const ParticleEffect& effect, const f32 time, ParticleFeed& feed)
    {
        const auto& emitters = effect.GetEmitters();
        const auto alive = [&](const ParticleFeedSpawn& spawn)
        {
            return spawn.Emitter < emitters.size() && emitters[spawn.Emitter].Fed &&
                   spawn.Time + emitters[spawn.Emitter].Lifetime.y > time;
        };
        // Spawns arrive in time order, but emitters differ in lifetime: the front goes only while
        // it is dead, so a longer-lived spawn behind a shorter-lived one waits for it.
        usize dead = 0;
        while (dead < feed.Spawns.size() && !alive(feed.Spawns[dead]))
        {
            ++dead;
        }
        if (dead > 0)
        {
            feed.Spawns.erase(feed.Spawns.begin(), feed.Spawns.begin() + static_cast<isize>(dead));
            feed.First += dead;
        }
    }

    void RestartParticleEffect(ParticleEffectInstance& instance)
    {
        instance.Time = 0.0f;
        instance.StopTime = -1.0f;
        instance.Finished = false;
        instance.Anchored = false;
        ++instance.Playback;
    }

    optional<Light> ParticleFlashLight(const ParticleEffect& effect, const f32 time,
                                       const f32 scale)
    {
        vec3 weighted(0.0f);
        f32 intensity = 0.0f;
        f32 range = 0.0f;
        f32 radius = 0.0f;
        bool live = false;
        for (const ParticleFlashDesc& flash : effect.GetFlashes())
        {
            if (!(time >= flash.Time && time < flash.Time + flash.Duration))
            {
                continue;
            }
            const f32 u = (time - flash.Time) / flash.Duration;
            const f32 lumens = flash.Intensity * EvaluateParticleCurve(flash.Curve, u, 1.0f);
            weighted += flash.Color * lumens;
            intensity += lumens;
            range = std::max(range, flash.Range);
            radius = std::max(radius, flash.Radius);
            live = true;
        }
        if (!live)
        {
            return std::nullopt;
        }
        Light light{.Type = LightType::Point};
        light.Color = intensity > 0.0f ? weighted / intensity : vec3(1.0f);
        light.Intensity = intensity;
        light.Range = range * scale;
        light.Radius = radius * scale;
        return light;
    }

    void ParticleEffectSystem::OnUpdate(Scene& scene, const f32 delta,
                                        const SystemContext& /*context*/)
    {
        // Light changes are applied after the walk, since adding or removing a component while
        // iterating a view would invalidate it.
        vector<std::pair<Entity, optional<Light>>> lights;
        for (auto [entity, instance] : scene.View<ParticleEffectInstance>())
        {
            const ParticleEffect* effect = instance.Effect.Get();
            if (effect == nullptr)
            {
                continue;
            }
            if (!instance.Anchored)
            {
                LatchParticleEffect(scene, entity, instance);
            }
            if (!instance.Finished)
            {
                if (!instance.Emitting && instance.StopTime < 0.0f)
                {
                    instance.StopTime = instance.Time;
                }
                instance.Time += delta;
                if (auto* feed = scene.TryGet<ParticleFeed>(entity))
                {
                    PruneParticleFeed(*effect, instance.Time, *feed);
                }
                instance.Finished =
                    instance.Time >= ParticleEffectEndTime(*effect, instance.StopTime);
            }
            if (!effect->GetFlashes().empty())
            {
                lights.emplace_back(
                    entity, instance.Finished
                                ? std::nullopt
                                : ParticleFlashLight(*effect, instance.Time, instance.Scale));
            }
        }

        for (const auto& [entity, light] : lights)
        {
            if (light)
            {
                if (auto* existing = scene.TryGet<Light>(entity))
                {
                    *existing = *light;
                }
                else
                {
                    scene.Add<Light>(entity, *light);
                }
            }
            else if (scene.Has<Light>(entity))
            {
                (void)scene.Remove<Light>(entity);
            }
        }
    }
}
