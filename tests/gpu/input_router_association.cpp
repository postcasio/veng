// InputRouter viewport-association cases (GPU band): the resolution path needs a real Viewport
// (Viewport::Create needs a live Context), so the id-keyed live-resolve behaviour is proven here.
// The router stores each association's ViewportId and resolves it against Context::GetViewportRegistry()
// every hit-test: a destroyed viewport's association becomes an inert no-op, a fresh viewport at a
// dead one's region inherits no seat (the ABA case), and a live-but-unregistered viewport still routes.

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/InputMappingContext.h>
#include <Veng/Input.h>
#include <Veng/Input/SeatFocusScope.h>
#include <Veng/InputRouter.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Renderer/ViewportRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    Unique<Viewport> CreateViewport(Context& context, AssetManager& assets, uvec2 extent)
    {
        return Viewport::Create({
            .Context = context,
            .Assets = assets,
            .Region = {.Offset = {0, 0}, .Extent = extent},
            .Settings = {},
            .Role = ViewportRole::Offscreen,
        });
    }

    // A resident AssetHandle<InputMappingContext> carrying the given non-zero id, built without an
    // AssetManager: a detached cache entry wired into a type-erased handle the way a prefab spawn
    // rehydrates one. SeatFocusScope reads the id's validity to decide whether to swap, so a
    // manufactured non-zero id is enough.
    AssetHandle<InputMappingContext> MakeContext(u64 id)
    {
        const Ref<InputMappingContext> resource = InputMappingContext::Create({}, {});
        auto entry = CreateRef<Detail::AssetCacheEntry>(
            Detail::AssetCacheEntry{.Id = AssetId{.Value = id},
                                    .Type = AssetType::InputMap,
                                    .Resource = Detail::RefAny(resource)});
        AssetHandle<InputMappingContext> handle;
        Detail::RehydrateHandleField(&handle, AssetId{.Value = id}, std::move(entry));
        return handle;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "input_router associations: a destroyed viewport's association routes nothing")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    Input input(nullptr);
    InputRouter router(nullptr, input, Context.GetViewportRegistry());

    constexpr Entity seat{.Index = 1, .Generation = 1};
    const ivec2 inside{16, 16};

    {
        const Unique<Viewport> viewport = CreateViewport(Context, assets, {32, 32});
        router.AssociateViewportSeat(*viewport, seat);

        // A live association routes the pointer over its region to the seat.
        const PointerRouting live = router.ResolvePointer(inside, false, Entity::Null);
        CHECK(live.Owner == seat);
        CHECK(router.ResolvePointerViewport(inside, false) == viewport.get());
    }

    // The viewport is gone: its stale association resolves to nothing, so the pointer over the old
    // region routes nowhere and the scene-scoping companion returns null — no dangling deref.
    const PointerRouting stale = router.ResolvePointer(inside, false, Entity::Null);
    CHECK(stale.Owner == Entity::Null);
    CHECK(router.ResolvePointerViewport(inside, false) == nullptr);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "input_router associations: a fresh viewport inherits no dead association (ABA)")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    Input input(nullptr);
    InputRouter router(nullptr, input, Context.GetViewportRegistry());

    constexpr Entity seatA{.Index = 1, .Generation = 1};
    constexpr Entity seatB{.Index = 2, .Generation = 1};
    const ivec2 inside{16, 16};

    ViewportId deadId{};
    {
        const Unique<Viewport> first = CreateViewport(Context, assets, {32, 32});
        deadId = first->GetId();
        router.AssociateViewportSeat(*first, seatA);
        CHECK(router.ResolvePointer(inside, false, Entity::Null).Owner == seatA);
    }

    // A fresh viewport covering the same region carries a distinct id (ids are never reused), so
    // seatA's stale association can never transfer to it — before it is associated the pointer over
    // the region routes nowhere.
    const Unique<Viewport> second = CreateViewport(Context, assets, {32, 32});
    CHECK_FALSE(second->GetId() == deadId);
    CHECK(router.ResolvePointer(inside, false, Entity::Null).Owner == Entity::Null);

    // Associating the fresh viewport routes to its own seat, not the dead viewport's.
    router.AssociateViewportSeat(*second, seatB);
    CHECK(router.ResolvePointer(inside, false, Entity::Null).Owner == seatB);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "input_router associations: a live-but-unregistered viewport still routes")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    Input input(nullptr);
    InputRouter router(nullptr, input, Context.GetViewportRegistry());

    constexpr Entity seat{.Index = 3, .Generation = 1};
    const ivec2 inside{20, 10};

    // This viewport is never handed to any drive-list — it is only minted in the Context registry.
    // The router resolves against construction lifetime, not drive-list membership, so it routes.
    const Unique<Viewport> viewport = CreateViewport(Context, assets, {40, 20});
    router.AssociateViewportSeat(*viewport, seat);

    const PointerRouting routing = router.ResolvePointer(inside, false, Entity::Null);
    CHECK(routing.Owner == seat);
    CHECK(router.ResolvePointerViewport(inside, false) == viewport.get());
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "input_router associations: a SeatFocusScope whose viewport dies mid-scope restores cleanly")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity seatEntity = scene->CreateEntity();
    scene->Add<Viewer>(seatEntity);
    scene->Add<PlayerInput>(seatEntity);
    auto& stack = scene->Add<InputContextStack>(seatEntity);
    stack.Active.push_back(MakeContext(0xAA11));

    const InputSeat seat = ResolveInputSeat(scene.get());
    REQUIRE(seat.Viewer == seatEntity);

    Input input(nullptr);
    InputRouter router(nullptr, input, Context.GetViewportRegistry());

    {
        Unique<Viewport> viewport = CreateViewport(Context, assets, {32, 32});
        const SeatFocusScope scope(router, seat, viewport.get(), MakeContext(0xBB22));

        // The takeover is live: UI focus, the swapped UI context, and a routing association.
        CHECK(router.GetFocus(seatEntity) == InputFocus::UI);
        CHECK(scene->Get<InputContextStack>(seatEntity).Active[0].Id().Value == 0xBB22);
        CHECK(router.ResolvePointer({16, 16}, false, Entity::Null).Owner == seatEntity);

        // The viewport dies while the scope holds it — the scope's clear becomes an id no-op.
        viewport.reset();
        CHECK(router.ResolvePointer({16, 16}, false, Entity::Null).Owner == Entity::Null);
    }

    // Dropping the scope after the viewport died restores exactly as if the viewport survived: the
    // focus pops back to UI and the gameplay context is restored in place — no crash.
    CHECK(router.GetFocus(seatEntity) == InputFocus::UI);
    const InputContextStack& restored = scene->Get<InputContextStack>(seatEntity);
    REQUIRE(restored.Active.size() == 1);
    CHECK(restored.Active[0].Id().Value == 0xAA11);
}
