// Flipbook playback timing and the transient-effect pool, device-free: the frame a clip shows at a
// time, the duration override, when a one-shot finishes, and the pool's reuse, bound, and retire.

#include <doctest/doctest.h>

#include <cmath>

#include <Veng/Asset/Flipbook.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/EffectPool.h>
#include <Veng/Scene/FlipbookSystem.h>
#include <Veng/Scene/Scene.h>
#include "support/TestServices.h"

using namespace Veng;

TEST_CASE("A flipbook's frame at time t is floor(t * rate) wrapped when looping, clamped when not")
{
    const FlipbookClip looping{.FrameCount = 4, .Fps = 8.0f, .Loop = true};
    const FlipbookClip oneShot{.FrameCount = 4, .Fps = 8.0f, .Loop = false};
    const f32 rate = ResolveFlipbookRate(looping, 1.5f, 0.0f);
    CHECK(rate == doctest::Approx(12.0f));

    u32 mismatches = 0;
    for (u32 step = 0; step < 64; ++step)
    {
        const f32 t = static_cast<f32>(step) * 0.037f;
        const auto index = static_cast<u64>(std::floor(static_cast<f64>(t) * rate));
        mismatches += FlipbookFrameAt(looping, rate, t) != static_cast<u32>(index % 4) ? 1u : 0u;
        mismatches += FlipbookFrameAt(oneShot, rate, t) != std::min<u64>(index, 3) ? 1u : 0u;
    }
    CHECK(mismatches == 0);

    CHECK(FlipbookFrameAt(oneShot, rate, -1.0f) == 0);
    CHECK(FlipbookFrameAt(oneShot, rate, 1e9f) == 3);
}

TEST_CASE("A duration override fits the whole sequence to the stated seconds")
{
    // A slow twenty-second bake serving a one-and-a-half-second burst.
    const FlipbookClip clip{.FrameCount = 64, .Fps = 3.2f, .Loop = false};
    const f32 rate = ResolveFlipbookRate(clip, /*playbackRate=*/4.0f, /*durationOverride=*/1.5f);
    CHECK(rate == doctest::Approx(64.0f / 1.5f));

    CHECK(FlipbookFrameAt(clip, rate, 0.0f) == 0);
    CHECK(FlipbookFrameAt(clip, rate, 0.75f) == 32);
    CHECK_FALSE(IsFlipbookFinished(clip, rate, 1.49f));
    CHECK(IsFlipbookFinished(clip, rate, 1.5f));
}

TEST_CASE("A one-shot finishes after its last frame; a looping or stopped clip never does")
{
    const FlipbookClip oneShot{.FrameCount = 4, .Fps = 10.0f, .Loop = false};
    const FlipbookClip looping{.FrameCount = 4, .Fps = 10.0f, .Loop = true};
    CHECK_FALSE(IsFlipbookFinished(oneShot, 10.0f, 0.39f));
    CHECK(FlipbookFrameAt(oneShot, 10.0f, 0.39f) == 3);
    CHECK(IsFlipbookFinished(oneShot, 10.0f, 0.4f));
    CHECK_FALSE(IsFlipbookFinished(looping, 10.0f, 1000.0f));
    CHECK_FALSE(IsFlipbookFinished(oneShot, 0.0f, 1000.0f));
}

namespace
{
    struct PoolScene
    {
        TypeRegistry Types;
        Unique<Scene> World;

        PoolScene()
        {
            RegisterBuiltinTypes(Types);
            World = Scene::Create(Types);
        }
    };

    EffectDesc Burst()
    {
        return EffectDesc{.Sprite = FlipbookSprite{.Size = 2.0f}};
    }
}

TEST_CASE("The effect pool reuses entities, never exceeds its cap, and returns finished effects")
{
    PoolScene fixture;
    Scene& scene = *fixture.World;
    EffectPool pool(EffectPoolInfo{.Capacity = 3});

    // Filling the pool stands distinct Local-tier entities at their poses.
    vector<Entity> first;
    for (u32 i = 0; i < 3; ++i)
    {
        first.push_back(
            pool.Spawn(scene, Burst(), Transform{.Position = vec3(static_cast<f32>(i))}, 0.0f));
    }
    CHECK(pool.GetLiveCount() == 3);
    CHECK(first[0] != first[1]);
    CHECK(first[1] != first[2]);
    CHECK(scene.Get<Authority>(first[2]).Tier == Tier::Local);
    CHECK(scene.Get<Transform>(first[2]).Position.x == doctest::Approx(2.0f));

    // A spawn past the cap recycles the oldest rather than growing.
    const Entity recycled = pool.Spawn(scene, Burst(), Transform{}, 0.0f);
    CHECK(recycled == first[0]);
    CHECK(pool.GetLiveCount() == 3);
    CHECK(pool.GetFreeCount() == 0);

    // A finished sprite returns its entity to the free list, stripped of what it drew.
    scene.Get<FlipbookSprite>(first[1]).Finished = true;
    pool.Update(scene, 0.016f);
    CHECK(pool.GetLiveCount() == 2);
    CHECK(pool.GetFreeCount() == 1);
    CHECK_FALSE(pool.IsLive(first[1]));
    CHECK_FALSE(scene.Has<FlipbookSprite>(first[1]));

    // The next spawn takes the freed entity, and fresh playback state rides with it.
    const Entity reused = pool.Spawn(scene, Burst(), Transform{}, 0.0f);
    CHECK(reused == first[1]);
    CHECK_FALSE(scene.Get<FlipbookSprite>(reused).Finished);
    CHECK(scene.Get<FlipbookSprite>(reused).Time == 0.0f);

    // Many more spawns than the cap still stand no more than it.
    for (u32 i = 0; i < 20; ++i)
    {
        (void)pool.Spawn(scene, Burst(), Transform{}, 0.0f);
    }
    u32 sprites = 0;
    for ([[maybe_unused]] auto [entity, sprite] : scene.View<FlipbookSprite>())
    {
        ++sprites;
    }
    CHECK(sprites == 3);
    CHECK(pool.GetLiveCount() + pool.GetFreeCount() == 3);
}

TEST_CASE("A pooled effect retires at the end of its lifetime, taking its light with it")
{
    PoolScene fixture;
    Scene& scene = *fixture.World;
    EffectPool pool(EffectPoolInfo{.Capacity = 4});

    EffectDesc flash = Burst();
    flash.Light = Light{.Type = LightType::Point, .Intensity = 800.0f};
    const Entity effect = pool.Spawn(scene, flash, Transform{}, 0.25f);
    CHECK(scene.Has<Light>(effect));

    pool.Update(scene, 0.2f);
    CHECK(pool.IsLive(effect));
    pool.Update(scene, 0.1f);
    CHECK_FALSE(pool.IsLive(effect));
    CHECK_FALSE(scene.Has<Light>(effect));

    // A spawn with no light on the reused entity carries none.
    const Entity dark = pool.Spawn(scene, Burst(), Transform{}, 0.0f);
    CHECK(dark == effect);
    CHECK_FALSE(scene.Has<Light>(dark));
}

TEST_CASE("SpawnTransientEffect installs the scene's pool, and FlipbookSystem retires through it")
{
    PoolScene fixture;
    Scene& scene = *fixture.World;
    CHECK(scene.GetEffectPool() == nullptr);

    const Entity effect = SpawnTransientEffect(scene, Burst(), Transform{}, 0.5f);
    REQUIRE(scene.GetEffectPool() != nullptr);
    CHECK(scene.GetEffectPool()->GetCapacity() == DefaultEffectPoolCapacity);
    CHECK(scene.GetEffectPool()->IsLive(effect));

    // No flipbook is resident, so the sprite holds; the lifetime alone retires it.
    FlipbookSystem system;
    TestSupport::TestServices services;
    const SystemContext context = services.Make();
    system.OnUpdate(scene, 0.3f, context);
    CHECK(scene.Get<FlipbookSprite>(effect).Time == 0.0f);
    CHECK(scene.GetEffectPool()->IsLive(effect));
    system.OnUpdate(scene, 0.3f, context);
    CHECK_FALSE(scene.GetEffectPool()->IsLive(effect));
}
