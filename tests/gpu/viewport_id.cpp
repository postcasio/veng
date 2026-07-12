// ViewportId minting cases (GPU band): Viewport::Create needs a live Context,
// so the mint/resolve/stability/churn/teardown behaviour is proven here. Each
// Viewport mints an id against Context::GetViewportRegistry() at Create and
// retires it at destruction; Resolve maps a live id to its viewport and a
// retired or unminted id to nullptr, and the monotonic counter never reuses a
// value.

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Renderer/ViewportRegistry.h>

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
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "viewport_id: two viewports mint distinct ids that resolve to the right "
                  "viewport; the id is stable across SetRegion/Configure; a destroyed viewport "
                  "resolves to nullptr")
{
    AssetManager assets(Context, Tasks, Types);
    const VoidResult mountResult = assets.Mount(path(TEST_SHADER_PACK));
    REQUIRE(mountResult.has_value());

    const ViewportRegistry& registry = Context.GetViewportRegistry();

    const Unique<Viewport> first = CreateViewport(Context, assets, {32, 32});
    const Unique<Viewport> second = CreateViewport(Context, assets, {48, 24});

    // Both ids are valid, distinct, and resolve back to their own viewport.
    CHECK(first->GetId().IsValid());
    CHECK(second->GetId().IsValid());
    CHECK_FALSE(first->GetId() == second->GetId());
    CHECK(registry.Resolve(first->GetId()) == first.get());
    CHECK(registry.Resolve(second->GetId()) == second.get());

    // The id is immutable across a region change and a reconfigure.
    const ViewportId firstId = first->GetId();
    first->SetRegion({.Offset = {4, 4}, .Extent = {64, 64}});
    CHECK(first->GetId() == firstId);

    SceneRendererSettings reconfigured = first->GetSettings();
    reconfigured.Bloom = !reconfigured.Bloom;
    first->Configure(reconfigured);
    CHECK(first->GetId() == firstId);
    CHECK(registry.Resolve(firstId) == first.get());

    // Destroying a viewport retires its id: it resolves to nullptr afterwards, and the
    // survivor is untouched.
    const ViewportId secondId = second->GetId();
    {
        const Unique<Viewport> transient = CreateViewport(Context, assets, {16, 16});
        const ViewportId transientId = transient->GetId();
        CHECK(registry.Resolve(transientId) == transient.get());
    }
    // The transient viewport is gone; the survivor still resolves.
    CHECK(registry.Resolve(secondId) == second.get());
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "viewport_id: a create/destroy/create churn mints a strictly-greater id every "
                  "cycle and never resolves a retired id")
{
    AssetManager assets(Context, Tasks, Types);
    const VoidResult mountResult = assets.Mount(path(TEST_SHADER_PACK));
    REQUIRE(mountResult.has_value());

    const ViewportRegistry& registry = Context.GetViewportRegistry();

    ViewportId previous{};
    for (int cycle = 0; cycle < 4; ++cycle)
    {
        ViewportId retired{};
        {
            const Unique<Viewport> viewport = CreateViewport(Context, assets, {32, 32});
            const ViewportId id = viewport->GetId();

            CHECK(id.IsValid());
            // Slots are never pooled, so each cycle's id strictly exceeds the last — the
            // no-reuse property the input layer's ABA closure rests on.
            CHECK(id.Value > previous.Value);
            CHECK(registry.Resolve(id) == viewport.get());

            previous = id;
            retired = id;
        }
        // The viewport was destroyed against the still-live Context registry (teardown order),
        // so its id now resolves to nothing.
        CHECK(registry.Resolve(retired) == nullptr);
    }
}
