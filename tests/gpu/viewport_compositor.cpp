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

#include <optional>

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

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "viewport_compositor: ResolveTrackingLayouts re-resolves a Layout-carrying viewport's region "
    "and stamps its UI scale, leaving an absolute viewport untouched")
{
    AssetManager assets(Context, Tasks, Types);
    const VoidResult mountResult = assets.Mount(path(TEST_SHADER_PACK));
    REQUIRE(mountResult.has_value());

    ViewportCompositor compositor(Context);

    // The fixture's headless render extent is the layout resolution basis.
    const uvec2 renderExtent = Context.GetRenderExtent();

    // ResolveLayout is the single layout→pixel path: round(Layout · render extent).
    const ViewportRegion full =
        compositor.ResolveLayout({.Offset = {0.0f, 0.0f}, .Extent = {1.0f, 1.0f}});
    CHECK(full.Offset == ivec2{0, 0});
    CHECK(full.Extent == renderExtent);

    // A window-tracking viewport (right half) and an absolute viewport that never tracks.
    const ViewportLayout rightHalf{.Offset = {0.5f, 0.0f}, .Extent = {0.5f, 1.0f}};
    Unique<Viewport> tracking = CreatePresented(Context, assets, {8, 8});
    tracking->SetLayout(rightHalf);
    Unique<Viewport> absolute = CreatePresented(Context, assets, {20, 12});

    compositor.RegisterViewport(*tracking);
    compositor.RegisterViewport(*absolute);

    const ViewportRegion expected = compositor.ResolveLayout(rightHalf);

    // The resize reaction: the compositor re-resolves the tracking viewport's region from its Layout
    // and stamps its UI scale (1.0 headless), and leaves the absolute viewport's region alone.
    compositor.ResolveTrackingLayouts();
    CHECK(tracking->GetRegion().Offset == expected.Offset);
    CHECK(tracking->GetRegion().Extent == expected.Extent);
    CHECK(tracking->GetUiScale() == doctest::Approx(1.0f));
    CHECK(absolute->GetRegion().Offset == ivec2{0, 0});
    CHECK(absolute->GetRegion().Extent == uvec2{20, 12});

    // A stale region (a swapchain change) is re-resolved from the Layout, not from a per-frame apply:
    // clobber the region, run the reaction again, and it restores to the layout-resolved rectangle.
    tracking->SetRegion({.Offset = {3, 5}, .Extent = {7, 9}});
    compositor.ResolveTrackingLayouts();
    CHECK(tracking->GetRegion().Offset == expected.Offset);
    CHECK(tracking->GetRegion().Extent == expected.Extent);

    // Clearing the Layout makes the viewport absolute: the reaction no longer touches its region.
    tracking->SetLayout(std::nullopt);
    tracking->SetRegion({.Offset = {1, 2}, .Extent = {6, 4}});
    compositor.ResolveTrackingLayouts();
    CHECK(tracking->GetRegion().Offset == ivec2{1, 2});
    CHECK(tracking->GetRegion().Extent == uvec2{6, 4});
}
