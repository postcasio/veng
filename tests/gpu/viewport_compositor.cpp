// ViewportCompositor registration + teardown contract (GPU band): the compositor owns the
// render-order viewport drive-list independent of any Application. Viewport::Create needs a live
// Context, so the register/order/self-unregister and teardown behaviour is proven here against a
// standalone-constructed compositor borrowing the fixture's Context.
//
//  - RegisterViewport appends in registration order, which GetViewports() reports (the order the
//    render + gather walk consumes), and dropping a registered viewport self-unregisters it
//    order-preserving;
//  - tearing the viewports down while the Context is still alive retires each id against the live
//    Context registry and self-cleans the compositor's drive-list — no retire touches a destroyed
//    registry (the teardown-order invariant: viewports drop before the compositor before the
//    Context).

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Renderer/ViewportCompositor.h>
#include <Veng/Renderer/ViewportRegistry.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    Unique<Viewport> CreatePresented(Context& context, AssetManager& assets, uvec2 extent)
    {
        return Viewport::Create({
            .Context = context,
            .Assets = assets,
            .Region = {.Offset = {0, 0}, .Extent = extent},
            .Settings = {},
            .Role = ViewportRole::Presented,
        });
    }
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "viewport_compositor: RegisterViewport drives in registration order and a dropped "
    "viewport self-unregisters order-preserving")
{
    AssetManager assets(Context, Tasks, Types);
    const VoidResult mountResult = assets.Mount(path(TEST_SHADER_PACK));
    REQUIRE(mountResult.has_value());

    ViewportCompositor compositor(Context);
    CHECK(compositor.GetViewports().empty());

    const Unique<Viewport> first = CreatePresented(Context, assets, {32, 32});
    Unique<Viewport> second = CreatePresented(Context, assets, {40, 24});
    const Unique<Viewport> third = CreatePresented(Context, assets, {48, 16});

    compositor.RegisterViewport(*first);
    compositor.RegisterViewport(*second);
    compositor.RegisterViewport(*third);

    // Registration order is render order: the drive-list reports the three in the order registered.
    REQUIRE(compositor.GetViewports().size() == 3);
    CHECK(compositor.GetViewports()[0] == first.get());
    CHECK(compositor.GetViewports()[1] == second.get());
    CHECK(compositor.GetViewports()[2] == third.get());

    // Dropping the middle viewport self-unregisters it through its back-reference, order-preserving:
    // the survivors keep their relative order (a swap-and-pop would scramble the render order).
    second.reset();

    REQUIRE(compositor.GetViewports().size() == 2);
    CHECK(compositor.GetViewports()[0] == first.get());
    CHECK(compositor.GetViewports()[1] == third.get());
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "viewport_compositor: tearing viewports down against the live Context retires each "
    "id and empties the drive-list, touching no destroyed registry")
{
    AssetManager assets(Context, Tasks, Types);
    const VoidResult mountResult = assets.Mount(path(TEST_SHADER_PACK));
    REQUIRE(mountResult.has_value());

    const ViewportRegistry& registry = Context.GetViewportRegistry();

    // The compositor is declared before the viewports, so the viewports destruct first (reverse
    // scope order): they self-unregister from the live compositor drive-list and retire their ids
    // against the still-live Context registry — the Application teardown order (owned viewports <
    // compositor < Context).
    ViewportCompositor compositor(Context);

    Unique<Viewport> first = CreatePresented(Context, assets, {32, 32});
    Unique<Viewport> second = CreatePresented(Context, assets, {48, 24});

    const ViewportId firstId = first->GetId();
    const ViewportId secondId = second->GetId();

    compositor.RegisterViewport(*first);
    compositor.RegisterViewport(*second);

    // Registered and live: each id resolves to its viewport through the Context registry.
    REQUIRE(compositor.GetViewports().size() == 2);
    CHECK(registry.Resolve(firstId) == first.get());
    CHECK(registry.Resolve(secondId) == second.get());

    // Drop one viewport while the Context (and its registry) is alive: its id retires and the
    // drive-list self-cleans, with the survivor untouched.
    first.reset();
    CHECK(registry.Resolve(firstId) == nullptr);
    REQUIRE(compositor.GetViewports().size() == 1);
    CHECK(compositor.GetViewports()[0] == second.get());
    CHECK(registry.Resolve(secondId) == second.get());

    // Drop the last viewport: its id retires against the still-live registry and the drive-list
    // empties, so the compositor holds no dangling pointer when it destructs after this scope.
    second.reset();
    CHECK(registry.Resolve(secondId) == nullptr);
    CHECK(compositor.GetViewports().empty());
}
