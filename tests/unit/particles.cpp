// Particle effects, device-free: the closed-form trajectory, the spawn schedule and its cursors, the
// spawn state an index derives, the instance's lifetime on the CPU, the pool's retire, the anchor
// under a floating origin, a fed emitter's spawns issued once each and pruned once dead, the baked
// over-life tables, the gather's budgets, and its wait on a flipbook still loading.

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <limits>
#include <set>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/ParticleEffect.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/EffectPool.h>
#include <Veng/Scene/ParticleEffectSystem.h>
#include <Veng/Scene/Scene.h>

#include "Renderer/ParticleGather.h"
#include "Renderer/ParticleMath.h"
#include "support/TestServices.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    struct ParticleScene
    {
        TypeRegistry Types;
        Unique<Scene> World;

        ParticleScene()
        {
            RegisterBuiltinTypes(Types);
            World = Scene::Create(Types);
        }
    };

    ParticleEmitterDesc Streak()
    {
        return ParticleEmitterDesc{
            .Render = ParticleRender::Streak,
            .Blend = ParticleBlend::Additive,
            .Lifetime = vec2(5.0f),
            .Speed = vec2(1.0f, 2.0f),
        };
    }

    AssetHandle<ParticleEffect> MakeEffect(vector<ParticleEmitterDesc> emitters,
                                           vector<ParticleFlashDesc> flashes = {})
    {
        return AssetManager::Adopt(ParticleEffect::Create(
            {.Name = "test", .Emitters = std::move(emitters), .Flashes = std::move(flashes)}));
    }

    // An anchored instance on a fresh entity.
    Entity Play(Scene& scene, const AssetHandle<ParticleEffect>& effect, const u32 seed = 7)
    {
        const Entity entity = scene.CreateEntity();
        scene.Add<Transform>(entity);
        ParticleEffectInstance instance{.Effect = effect, .Seed = seed};
        LatchParticleEffect(scene, entity, instance);
        scene.Add<ParticleEffectInstance>(entity, instance);
        return entity;
    }

    // The indices a frame's ranges issue.
    void Collect(const ParticleFramePlan& plan, std::multiset<u32>& indices)
    {
        for (const GpuParticleSpawnRange& range : plan.Ranges)
        {
            for (u32 i = 0; i < range.Count; ++i)
            {
                indices.insert(range.FirstIndex + i);
            }
        }
    }

    // Issues every index a playback spawns across the given gather times.
    std::multiset<u32> IssueAcross(const AssetHandle<ParticleEffect>& effect,
                                   const vector<f32>& times, vector<u32>* perFrame = nullptr)
    {
        ParticleScene fixture;
        const Entity entity = Play(*fixture.World, effect);
        ParticleGather gather;
        ParticleFramePlan plan;
        std::multiset<u32> indices;
        for (const f32 time : times)
        {
            fixture.World->Get<ParticleEffectInstance>(entity).Time = time;
            gather.Gather(*fixture.World, dvec3(0.0), DefaultParticleCapacity, plan);
            if (perFrame != nullptr)
            {
                perFrame->push_back(plan.SpawnTotal);
            }
            Collect(plan, indices);
        }
        return indices;
    }

    bool SameState(const ParticleSpawnState& a, const ParticleSpawnState& b)
    {
        return a.Offset == b.Offset && a.Velocity == b.Velocity && a.Lifetime == b.Lifetime &&
               a.Random == b.Random && a.Spin == b.Spin;
    }
}

TEST_CASE("With no acceleration and no drag a particle's path is exactly p0 + v0 t")
{
    const vec3 p0(1.5f, -2.25f, 3.0f);
    const vec3 v0(-4.0f, 0.75f, 12.5f);
    u32 mismatches = 0;
    for (const f32 t : {0.0f, 1.0f / 60.0f, 1.0f, 37.5f, 1000.0f})
    {
        mismatches += ParticleOffsetAt(p0, v0, vec3(0.0f), 0.0f, t) != p0 + v0 * t ? 1u : 0u;
        mismatches += ParticleVelocityAt(v0, vec3(0.0f), 0.0f, t) != v0 ? 1u : 0u;
    }
    CHECK(mismatches == 0);

    // A constant acceleration adds a t^2 / 2, to f32 precision of the double reference.
    const vec3 a(0.0f, -9.81f, 2.0f);
    f64 worst = 0.0;
    for (const f32 t : {0.5f, 3.0f, 40.0f})
    {
        const dvec3 reference = dvec3(p0) + dvec3(v0) * f64(t) + 0.5 * dvec3(a) * f64(t) * f64(t);
        const dvec3 got(ParticleOffsetAt(p0, v0, a, 0.0f, t));
        worst = std::max(worst, glm::length(got - reference) / glm::length(reference));
    }
    CHECK(worst < 1e-6);
}

TEST_CASE("Linear drag only slows a particle and bounds how far it travels")
{
    const vec3 p0(0.0f);
    const vec3 v0(3.0f, -4.0f, 12.0f);
    const f32 speed0 = glm::length(v0);
    u32 violations = 0;
    for (const f32 k : {0.1f, 2.0f})
    {
        f32 previous = speed0;
        for (u32 step = 1; step <= 8; ++step)
        {
            const f32 t = 0.4f * static_cast<f32>(step * step);
            const f32 speed = glm::length(ParticleVelocityAt(v0, vec3(0.0f), k, t));
            violations += speed < previous ? 0u : 1u;
            previous = speed;
            violations +=
                glm::length(ParticleOffsetAt(p0, v0, vec3(0.0f), k, t)) <= speed0 / k ? 0u : 1u;
        }
    }
    CHECK(violations == 0);

    // A vanishing drag takes the series branch and meets the drag-free path.
    const vec3 a(0.0f, -9.81f, 0.0f);
    const vec3 dragFree = ParticleOffsetAt(p0, v0, a, 0.0f, 2.5f);
    const vec3 tiny = ParticleOffsetAt(p0, v0, a, 1e-7f, 2.5f);
    CHECK(glm::length(tiny - dragFree) / glm::length(dragFree) < 1e-5f);
}

TEST_CASE("A burst issues exactly its particles once, however the playback is partitioned")
{
    ParticleEmitterDesc burst = Streak();
    burst.BurstCount = 7;
    burst.BurstTime = 0.3f;
    const AssetHandle<ParticleEffect> effect = MakeEffect({burst});

    const std::multiset<u32> expected{0, 1, 2, 3, 4, 5, 6};
    vector<f32> even;
    for (u32 i = 0; i <= 5; ++i)
    {
        even.push_back(0.4f * static_cast<f32>(i));
    }
    const vector<f32> uneven{0.0f,  0.01f, 0.05f, 0.11f, 0.2f,  0.29f, 0.2999f,
                             0.31f, 0.5f,  0.9f,  1.3f,  1.71f, 2.0f};
    CHECK(IssueAcross(effect, {2.0f}) == expected);
    CHECK(IssueAcross(effect, even) == expected);

    vector<u32> perFrame;
    CHECK(IssueAcross(effect, uneven, &perFrame) == expected);
    // Only the frame whose window takes in 0.3 issues anything.
    u32 framesIssuing = 0;
    for (usize i = 0; i < perFrame.size(); ++i)
    {
        if (perFrame[i] > 0)
        {
            ++framesIssuing;
            CHECK(uneven[i] >= 0.3f);
            CHECK(uneven[i - 1] < 0.3f);
        }
    }
    CHECK(framesIssuing == 1);
}

TEST_CASE("A rate issues rate times the window, a bounded one exactly its count, and stops")
{
    ParticleEmitterDesc rate = Streak();
    rate.RatePerSecond = 30.0f;
    const AssetHandle<ParticleEffect> unbounded = MakeEffect({rate});
    for (const f32 window : {0.1f, 0.5f, 1.7f})
    {
        const auto issued = static_cast<f32>(IssueAcross(unbounded, {0.0f, window}).size());
        CHECK(std::abs(issued - 30.0f * window) <= 1.0f);
    }

    rate.RateDuration = 0.45f;
    const AssetHandle<ParticleEffect> bounded = MakeEffect({rate});
    CHECK(IssueAcross(bounded, {0.0f, 0.1f, 0.33f, 2.0f, 4.0f}).size() == 14);

    // Once emission stops, nothing past the stop time is issued.
    ParticleScene fixture;
    const Entity entity = Play(*fixture.World, unbounded);
    ParticleGather gather;
    ParticleFramePlan plan;
    auto& instance = fixture.World->Get<ParticleEffectInstance>(entity);
    instance.Time = 1.0f;
    gather.Gather(*fixture.World, dvec3(0.0), DefaultParticleCapacity, plan);
    instance.StopTime = 1.0f;
    instance.Time = 3.0f;
    gather.Gather(*fixture.World, dvec3(0.0), DefaultParticleCapacity, plan);
    CHECK(plan.SpawnTotal == 0);
}

TEST_CASE("A particle's spawn state is a pure function of its seed, emitter and index")
{
    ParticleEmitterDesc emitter = Streak();
    emitter.Shape = ParticleShape::Sphere;
    emitter.ShapeRadius = 2.0f;
    emitter.NoiseAmplitude = 0.5f;
    emitter.Spin = vec2(-90.0f, 90.0f);
    const ParticleSpawnParams params = MakeParticleSpawnParams(emitter);
    const ParticleSpawnInstance instance{.InheritVelocity = vec3(1.0f), .Seed = 11, .Scale = 2.0f};

    u32 differing = 0;
    u32 sameAcrossSeeds = 0;
    for (u32 index = 0; index < 16; ++index)
    {
        differing += SameState(MakeParticleSpawnState(params, instance, 3, index),
                               MakeParticleSpawnState(params, instance, 3, index))
                         ? 0u
                         : 1u;
        ParticleSpawnInstance reseeded = instance;
        reseeded.Seed = 12;
        sameAcrossSeeds += SameState(MakeParticleSpawnState(params, instance, 3, index),
                                     MakeParticleSpawnState(params, reseeded, 3, index))
                               ? 1u
                               : 0u;
    }
    CHECK(differing == 0);
    CHECK(sameAcrossSeeds == 0);

    // Two renderers at different cadences issue the same indices, so they derive the same states.
    ParticleEmitterDesc rate = Streak();
    rate.RatePerSecond = 40.0f;
    rate.RateDuration = 1.0f;
    const AssetHandle<ParticleEffect> effect = MakeEffect({rate});
    CHECK(IssueAcross(effect, {0.0f, 0.016f, 0.4f, 0.41f, 1.0f, 2.0f}) ==
          IssueAcross(effect, {0.25f, 0.75f, 2.0f}));
}

TEST_CASE("A particle picks among its emitter's flipbooks uniformly, and always 0 from one")
{
    constexpr u32 Keys = 4096;
    for (u32 count = 0; count <= MaxParticleFlipbookVariants; ++count)
    {
        std::array<u32, MaxParticleFlipbookVariants> picks{};
        u32 outOfRange = 0;
        for (u32 index = 0; index < Keys; ++index)
        {
            const u32 variant = ParticleVariantOf(ParticleKey(7, 2, index), count);
            outOfRange += variant >= std::max(count, 1u) ? 1u : 0u;
            ++picks[std::min(variant, MaxParticleFlipbookVariants - 1u)];
        }
        CHECK(outOfRange == 0);
        const u32 buckets = std::max(count, 1u);
        const f32 expected = static_cast<f32>(Keys) / static_cast<f32>(buckets);
        u32 leastPicked = Keys;
        u32 mostPicked = 0;
        for (u32 v = 0; v < buckets; ++v)
        {
            leastPicked = std::min(leastPicked, picks[v]);
            mostPicked = std::max(mostPicked, picks[v]);
        }
        CHECK(static_cast<f32>(leastPicked) > 0.85f * expected);
        CHECK(static_cast<f32>(mostPicked) < 1.15f * expected);
    }
}

TEST_CASE("Spawn shapes, the cone and the self-similar speed hold")
{
    const quat orientation = glm::angleAxis(0.7f, glm::normalize(vec3(1.0f, 2.0f, 0.5f)));
    const ParticleSpawnInstance instance{
        .Orientation = vec4(orientation.x, orientation.y, orientation.z, orientation.w), .Seed = 5};

    ParticleEmitterDesc emitter = Streak();
    emitter.ShapeRadius = 3.0f;
    emitter.ConeAxis = vec3(0.0f, 0.0f, 2.0f);
    emitter.ConeAngle = 25.0f;
    emitter.Speed = vec2(4.0f);
    emitter.SpeedByRadius = 1.0f;

    emitter.Shape = ParticleShape::Shell;
    const ParticleSpawnParams shell = MakeParticleSpawnParams(emitter);
    emitter.Shape = ParticleShape::Sphere;
    const ParticleSpawnParams sphere = MakeParticleSpawnParams(emitter);
    const vec3 axis = orientation * vec3(0.0f, 0.0f, 1.0f);
    const f32 cosLimit = std::cos(glm::radians(25.0f)) - 1e-5f;

    f32 shellError = 0.0f;
    f32 sphereExcess = 0.0f;
    f32 minCos = 1.0f;
    f32 minRatio = std::numeric_limits<f32>::max();
    f32 maxRatio = 0.0f;
    for (u32 index = 0; index < 16; ++index)
    {
        const ParticleSpawnState onShell = MakeParticleSpawnState(shell, instance, 0, index);
        const ParticleSpawnState inSphere = MakeParticleSpawnState(sphere, instance, 0, index);
        shellError = std::max(shellError, std::abs(glm::length(onShell.Offset) - 3.0f));
        sphereExcess = std::max(sphereExcess, glm::length(inSphere.Offset) - 3.0f);
        minCos = std::min(minCos, glm::dot(glm::normalize(onShell.Offset), axis));
        const f32 ratio = glm::length(inSphere.Velocity) / glm::length(inSphere.Offset);
        minRatio = std::min(minRatio, ratio);
        maxRatio = std::max(maxRatio, ratio);
    }
    CHECK(shellError < 1e-5f);
    CHECK(sphereExcess <= 1e-5f);
    CHECK(minCos >= cosLimit);
    CHECK((maxRatio - minRatio) / minRatio < 1e-5f);
}

TEST_CASE("Scaling an instance scales every particle's path and leaves its timing alone")
{
    ParticleEmitterDesc emitter = Streak();
    emitter.Shape = ParticleShape::Sphere;
    emitter.ShapeRadius = 1.5f;
    emitter.Acceleration = vec3(0.0f, -2.0f, 0.5f);
    emitter.Drag = 0.7f;
    const ParticleSpawnParams params = MakeParticleSpawnParams(emitter);
    constexpr f32 Scale = 2.5f;

    f32 worst = 0.0f;
    u32 timingMoved = 0;
    for (u32 index = 0; index < 8; ++index)
    {
        const ParticleSpawnState unit = MakeParticleSpawnState(params, {.Seed = 4}, 0, index);
        const ParticleSpawnState scaled =
            MakeParticleSpawnState(params, {.Seed = 4, .Scale = Scale}, 0, index);
        timingMoved += unit.Lifetime != scaled.Lifetime || unit.Spin != scaled.Spin ? 1u : 0u;
        for (const f32 age : {0.0f, 0.3f, 2.0f})
        {
            const vec3 a = ParticleOffsetAt(unit.Offset, unit.Velocity, emitter.Acceleration,
                                            emitter.Drag, age);
            const vec3 b = ParticleOffsetAt(scaled.Offset, scaled.Velocity,
                                            emitter.Acceleration * Scale, emitter.Drag, age);
            worst = std::max(worst, glm::length(b - a * Scale) / std::max(glm::length(b), 1e-3f));
        }
    }
    CHECK(timingMoved == 0);
    CHECK(worst < 1e-5f);
}

TEST_CASE("A renderer meeting a long-running effect issues only the particles still alive")
{
    ParticleEmitterDesc rate = Streak();
    rate.RatePerSecond = 25.0f;
    rate.Lifetime = vec2(1.0f, 2.0f);
    const AssetHandle<ParticleEffect> effect = MakeEffect({rate});

    ParticleScene fixture;
    const Entity entity = Play(*fixture.World, effect, 9);
    fixture.World->Get<ParticleEffectInstance>(entity).Time = 100.0f;
    ParticleGather gather;
    ParticleFramePlan plan;
    gather.Gather(*fixture.World, dvec3(0.0), DefaultParticleCapacity, plan);

    const ParticleEmitterDesc& emitter = effect.Get()->GetEmitters()[0];
    const ParticleSpawnParams params = MakeParticleSpawnParams(emitter);
    const ParticleSpawnInstance instance{.Seed = 9};
    u32 longDead = 0;
    u32 alive = 0;
    std::multiset<u32> indices;
    Collect(plan, indices);
    for (const u32 index : indices)
    {
        const f32 spawn = ParticleSpawnTime(emitter, index);
        longDead += spawn + emitter.Lifetime.y <= 100.0f ? 1u : 0u;
        alive +=
            100.0f - spawn < MakeParticleSpawnState(params, instance, 0, index).Lifetime ? 1u : 0u;
    }
    CHECK_FALSE(indices.empty());
    CHECK(longDead == 0);
    CHECK(plan.AliveBound >= alive);
}

TEST_CASE("An instance finishes exactly when everything it spawned has died")
{
    ParticleEmitterDesc burst = Streak();
    burst.BurstCount = 4;
    burst.Lifetime = vec2(0.5f, 1.0f);
    const AssetHandle<ParticleEffect> effect = MakeEffect({burst});
    CHECK(ParticleEffectEndTime(*effect.Get(), -1.0f) == 1.0f);

    ParticleScene fixture;
    Scene& scene = *fixture.World;
    const Entity entity = Play(scene, effect);
    ParticleEffectSystem system;
    TestSupport::TestServices services;
    const SystemContext context = services.Make();
    system.OnUpdate(scene, 0.99f, context);
    CHECK_FALSE(scene.Get<ParticleEffectInstance>(entity).Finished);
    system.OnUpdate(scene, 0.01f, context);
    CHECK(scene.Get<ParticleEffectInstance>(entity).Finished);

    // Every particle a bounded effect spawns is dead by its end time.
    ParticleEmitterDesc rate = Streak();
    rate.BurstCount = 3;
    rate.BurstTime = 0.2f;
    rate.RatePerSecond = 12.0f;
    rate.RateStart = 0.1f;
    rate.RateDuration = 1.3f;
    rate.Lifetime = vec2(0.3f, 0.9f);
    const AssetHandle<ParticleEffect> bounded = MakeEffect({rate});
    const f32 end = ParticleEffectEndTime(*bounded.Get(), -1.0f);
    const ParticleSpawnParams params = MakeParticleSpawnParams(rate);
    const auto total = static_cast<u32>(3 + std::ceil(1.3 * 12.0));
    f32 latest = 0.0f;
    for (u32 index = 0; index < total; ++index)
    {
        latest =
            std::max(latest, ParticleSpawnTime(rate, index) +
                                 MakeParticleSpawnState(params, {.Seed = 3}, 0, index).Lifetime);
    }
    CHECK(latest <= end);

    // An unbounded rate never ends while emitting, and ends within a lifetime of stopping.
    ParticleEmitterDesc forever = Streak();
    forever.RatePerSecond = 10.0f;
    forever.Lifetime = vec2(0.5f, 2.0f);
    const AssetHandle<ParticleEffect> unbounded = MakeEffect({forever});
    CHECK(std::isinf(ParticleEffectEndTime(*unbounded.Get(), -1.0f)));
    const f32 stopped = ParticleEffectEndTime(*unbounded.Get(), 4.05f);
    CHECK(stopped <= 4.05f + 2.0f);
    CHECK(stopped > 4.05f + 2.0f - 0.1f);
}

TEST_CASE("The pool retires a particle effect when it finishes and lights only its flash")
{
    ParticleEmitterDesc burst = Streak();
    burst.BurstCount = 2;
    burst.Lifetime = vec2(0.5f);
    const ParticleFlashDesc flash{
        .Time = 0.1f, .Duration = 0.2f, .Intensity = 500.0f, .Range = 4.0f};
    const AssetHandle<ParticleEffect> effect = MakeEffect({burst}, {flash});

    ParticleScene fixture;
    Scene& scene = *fixture.World;
    EffectPool pool(EffectPoolInfo{.Capacity = 2});
    const EffectDesc desc{.Particles = ParticleEffectInstance{.Effect = effect}};
    const Entity entity = pool.Spawn(scene, desc, Transform{.Position = vec3(1.0f)}, 0.0f);
    CHECK(scene.Get<ParticleEffectInstance>(entity).Anchored);

    ParticleEffectSystem system;
    TestSupport::TestServices services;
    const SystemContext context = services.Make();
    const auto step = [&](const f32 delta)
    {
        system.OnUpdate(scene, delta, context);
        pool.Update(scene, delta);
    };
    step(0.05f);
    CHECK_FALSE(scene.Has<Light>(entity));
    step(0.1f);
    CHECK(scene.Has<Light>(entity));
    CHECK(scene.Get<Light>(entity).Intensity == doctest::Approx(500.0f));
    step(0.2f);
    CHECK_FALSE(scene.Has<Light>(entity));
    CHECK(pool.IsLive(entity));
    step(0.2f);
    CHECK_FALSE(pool.IsLive(entity));
    CHECK_FALSE(scene.Has<ParticleEffectInstance>(entity));

    // The recycled entity plays anew: a different playback, which a renderer tracks as a new
    // instance whose slot generation moves.
    ParticleGather gather;
    ParticleFramePlan plan;
    const Entity first = pool.Spawn(scene, desc, Transform{}, 0.0f);
    const u32 firstPlayback = scene.Get<ParticleEffectInstance>(first).Playback;
    gather.Gather(scene, dvec3(0.0), DefaultParticleCapacity, plan);
    REQUIRE(plan.InstanceCount == 1);
    const u32 slot = plan.Ranges.front().InstanceEmitter & 0xFFFFu;
    const u32 generationBefore = plan.Instances[slot].Generation;
    pool.Retire(scene, first);
    const Entity second = pool.Spawn(scene, desc, Transform{}, 0.0f);
    CHECK(second == first);
    CHECK(scene.Get<ParticleEffectInstance>(second).Playback != firstPlayback);
    gather.Gather(scene, dvec3(0.0), DefaultParticleCapacity, plan);
    CHECK(plan.Instances[slot].Generation != generationBefore);
}

TEST_CASE("An anchor latches in the stable frame and holds against a moving origin")
{
    ParticleScene fixture;
    Scene& scene = *fixture.World;
    const dvec3 origin(1.0e7, -2.0e6, 4.0e6);
    const Entity originEntity = scene.CreateEntity();
    scene.Add<FloatingOrigin>(originEntity, FloatingOrigin{.Position = origin});

    ParticleEmitterDesc burst = Streak();
    burst.BurstCount = 1;
    const AssetHandle<ParticleEffect> effect = MakeEffect({burst});
    const vec3 position(12.5f, -3.25f, 40.0f);
    const Entity entity = scene.CreateEntity();
    scene.Add<Transform>(entity, Transform{.Position = position});
    ParticleEffectInstance instance{.Effect = effect};
    LatchParticleEffect(scene, entity, instance);
    CHECK(instance.Anchor == dvec3(position) + origin);
    scene.Add<ParticleEffectInstance>(entity, instance);

    // Re-centring moves the origin by d and the camera by -d; the anchor's eye offset holds.
    const vec3 eye(2.0f, 1.0f, -5.0f);
    ParticleGather gather;
    ParticleFramePlan plan;
    gather.Gather(scene, dvec3(eye), DefaultParticleCapacity, plan);
    const vec3 base = plan.Instances[0].AnchorEye;
    f32 worst = 0.0f;
    for (const f64 d : {0.3, 1234.567, 3.0e5})
    {
        const dvec3 shift(d, -0.5 * d, 0.25 * d);
        scene.Get<FloatingOrigin>(originEntity).Position = origin + shift;
        gather.Gather(scene, dvec3(eye) - shift, DefaultParticleCapacity, plan);
        worst = std::max(worst, glm::length(plan.Instances[0].AnchorEye - base));
    }
    CHECK(worst <= glm::length(base) * 1e-6f);
}

TEST_CASE("A fed emitter's spawns are each issued once, in sequence, as their time comes")
{
    ParticleScene fixture;
    Scene& scene = *fixture.World;
    ParticleEmitterDesc fed = Streak();
    fed.Fed = true;
    fed.Lifetime = vec2(1.0f);
    const AssetHandle<ParticleEffect> effect = MakeEffect({fed});
    CHECK(std::isinf(ParticleEffectEndTime(*effect.Get(), -1.0f)));
    CHECK(ParticleEffectEndTime(*effect.Get(), 2.0f) == 3.0f);

    const Entity entity = Play(scene, effect);
    const dvec3 anchor = scene.Get<ParticleEffectInstance>(entity).Anchor;
    auto& feed = scene.Add<ParticleFeed>(entity);
    for (const f32 time : {0.1f, 0.2f, 0.5f})
    {
        feed.Spawns.push_back({.Position = anchor + dvec3(10.0 * time, -2.0, 4.0),
                               .Velocity = vec3(0.0f, 1.0f, 0.0f),
                               .Time = time});
    }
    // A spawn naming no fed emitter draws nothing.
    feed.Spawns.push_back({.Time = 0.5f, .Emitter = 3});

    ParticleGather gather;
    ParticleFramePlan plan;
    scene.Get<ParticleEffectInstance>(entity).Time = 0.3f;
    gather.Gather(scene, dvec3(0.0), DefaultParticleCapacity, plan);
    REQUIRE(plan.FedSpawns.size() == 2);
    REQUIRE(plan.Ranges.size() == 1);
    CHECK((plan.Ranges[0].InstanceEmitter & ParticleRangeFed) != 0u);
    CHECK(plan.Ranges[0].Count == 2);
    CHECK(plan.SpawnTotal == 2);
    CHECK(plan.FedSpawns[0].Index == 0);
    CHECK(plan.FedSpawns[1].Index == 1);
    CHECK(glm::length(plan.FedSpawns[1].Offset - vec3(2.0f, -2.0f, 4.0f)) < 1.0e-5f);
    CHECK(plan.FedSpawns[1].Time == 0.2f);

    // Nothing twice, and the next as its time comes.
    gather.Gather(scene, dvec3(0.0), DefaultParticleCapacity, plan);
    CHECK(plan.FedSpawns.empty());
    scene.Get<ParticleEffectInstance>(entity).Time = 0.6f;
    gather.Gather(scene, dvec3(0.0), DefaultParticleCapacity, plan);
    REQUIRE(plan.FedSpawns.size() == 1);
    CHECK(plan.FedSpawns[0].Index == 2);

    // A renderer meeting the feed late issues only what is still alive.
    scene.Get<ParticleEffectInstance>(entity).Time = 1.25f;
    ParticleGather late;
    late.Gather(scene, dvec3(0.0), DefaultParticleCapacity, plan);
    REQUIRE(plan.FedSpawns.size() == 1);
    CHECK(plan.FedSpawns[0].Index == 2);

    // Pruning drops the dead from the front and keeps the sequence.
    PruneParticleFeed(*effect.Get(), 1.25f, feed);
    CHECK(feed.First == 2);
    REQUIRE(feed.Spawns.size() == 2);
    CHECK(feed.Spawns[0].Time == 0.5f);
}

TEST_CASE("The baked over-life tables keep their curves' ends and shape")
{
    ParticleEmitterDesc emitter = Streak();
    emitter.SizeCurve = {
        {.T = 0.1f, .Value = 0.25f}, {.T = 0.6f, .Value = 2.0f}, {.T = 0.9f, .Value = 3.5f}};
    emitter.OpacityCurve = {{.T = 0.4f, .Value = 0.7f}};
    emitter.TintCurve = {{.T = 0.0f, .Value = vec3(1.0f, 0.5f, 0.0f)},
                         {.T = 1.0f, .Value = vec3(0.0f, 0.5f, 1.0f)}};
    const AssetHandle<ParticleEffect> effect = MakeEffect({emitter});
    const std::span<const vec4> lut = effect.Get()->GetEmitterLut(0);
    REQUIRE(lut.size() == 2 * ParticleLutSamples);

    CHECK(lut.front().x == 0.25f);
    CHECK(lut[ParticleLutSamples - 1].x == 3.5f);
    CHECK(vec3(lut[ParticleLutSamples]) == vec3(1.0f, 0.5f, 0.0f));
    CHECK(vec3(lut.back()) == vec3(0.0f, 0.5f, 1.0f));
    u32 descents = 0;
    u32 opacityMoves = 0;
    u32 defaults = 0;
    for (u32 i = 0; i < ParticleLutSamples; ++i)
    {
        descents += i > 0 && lut[i].x < lut[i - 1].x ? 1u : 0u;
        opacityMoves += lut[i].y != 0.7f ? 1u : 0u;
        defaults += lut[i].z != 1.0f || lut[i].w != 0.0f ? 1u : 0u;
    }
    CHECK(descents == 0);
    CHECK(opacityMoves == 0);
    CHECK(defaults == 0);
}

TEST_CASE("The gather drops and counts what its budgets cannot hold")
{
    ParticleEmitterDesc burst = Streak();
    burst.BurstCount = 80;
    const AssetHandle<ParticleEffect> effect = MakeEffect({burst});

    ParticleScene fixture;
    Scene& scene = *fixture.World;
    (void)Play(scene, effect);
    ParticleGather gather;
    ParticleFramePlan plan;
    gather.Gather(scene, dvec3(0.0), 64, plan);
    CHECK(plan.SpawnTotal == 64);
    CHECK(plan.SpawnsDropped == 16);

    for (u32 i = 0; i < MaxParticleInstances; ++i)
    {
        (void)Play(scene, effect);
    }
    gather.Gather(scene, dvec3(0.0), DefaultParticleCapacity, plan);
    CHECK(plan.InstanceCount == MaxParticleInstances);
    CHECK(plan.InstancesDropped == 1);
}

TEST_CASE("The gather waits on an effect whose flipbook is still loading, and drops nothing")
{
    // A billboard whose flipbook is not resident cannot be drawn yet; that is a wait, not a budget.
    const AssetHandle<ParticleEffect> loading = MakeEffect({ParticleEmitterDesc{
        .Render = ParticleRender::Billboard, .BurstCount = 4, .Lifetime = vec2(5.0f)}});
    ParticleScene fixture;
    Scene& scene = *fixture.World;
    for (u32 i = 0; i < 3; ++i)
    {
        (void)Play(scene, loading);
    }
    ParticleGather gather;
    ParticleFramePlan plan;
    gather.Gather(scene, dvec3(0.0), DefaultParticleCapacity, plan);
    CHECK(plan.InstancesWaiting == 3);
    CHECK(plan.EmittersDropped == 0);
    CHECK(plan.Emitters.empty());
}
