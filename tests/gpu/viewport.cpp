// Viewport cases. The Viewport wraps a SceneRenderer with a region, a role, a
// per-frame ViewState, and the Execute + Sample-barrier pair. These prove the
// wrapper's contract on its own (no Application drive-list, no compositor): a
// Render over a one-mesh scene produces a correctly-sized output and a valid
// bindless handle, a region extent change resizes the output (and re-registers
// the handle), and a null-World Render is a no-op that leaves the output intact.

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Viewport.h>

#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // Builds a cube scene with a materialless MeshRenderer and returns the owning
    // mesh so the caller keeps it resident for the viewport's lifetime.
    Ref<Mesh> PopulateCubeScene(Context& context, AssetManager& assets, Scene& scene)
    {
        const Ref<Mesh> cube = Mesh::BuildSync(context, Primitives::Cube(1.0f), "Viewport Cube");

        const Entity entity = scene.CreateEntity();
        scene.Add<Transform>(entity);
        scene.Add<MeshRenderer>(entity).Mesh = assets.Adopt(cube);

        return cube;
    }

    CameraView FrontCamera(uvec2 extent)
    {
        CameraView camera;
        camera.SetPerspective(glm::radians(45.0f),
                              static_cast<f32>(extent.x) / static_cast<f32>(extent.y), 0.1f,
                              100.0f);
        camera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));
        return camera;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "viewport: Render produces a sampleable output and a valid handle; a region "
                  "extent change resizes both; a null-World Render is a no-op")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    const VoidResult mountResult = assets.Mount(path(TEST_SHADER_PACK));
    REQUIRE(mountResult.has_value());

    constexpr uvec2 initialExtent{64, 48};

    const Unique<Scene> scene = Scene::Create(Types);
    const Ref<Mesh> cube = PopulateCubeScene(Context, assets, *scene);

    const Unique<Viewport> viewport = Viewport::Create({
        .Context = Context,
        .Assets = assets,
        .Region = {.Offset = {16, 8}, .Extent = initialExtent},
        // ColorFormat left Undefined: it must resolve to the window's output format.
        .Settings = {},
        .Role = ViewportRole::Offscreen,
    });

    // The region round-trips, and the role is what Create was given.
    CHECK(viewport->GetRegion().Offset == ivec2{16, 8});
    CHECK(viewport->GetRegion().Extent == initialExtent);
    CHECK(viewport->GetRole() == ViewportRole::Offscreen);

    // The output is a valid view at the region's extent, in the resolved (window) format.
    const Ref<ImageView> output = viewport->GetOutput();
    REQUIRE(output != nullptr);
    CHECK(output->GetImage()->GetWidth() == initialExtent.x);
    CHECK(output->GetImage()->GetHeight() == initialExtent.y);
    CHECK(output->GetImage()->GetFormat() == Context.GetOutputFormat());

    // The handle is registered at Create, before any Render.
    CHECK(viewport->GetOutputHandle().IsValid());

    // A null World (no ViewState bound) renders nothing: the output is untouched.
    Context.ImmediateCommands([&](CommandBuffer& cmd) { viewport->Render(cmd); });
    CHECK(viewport->GetOutput().get() == output.get());

    // Binding a ViewState + a fixed camera and rendering drives the Execute + Sample
    // barrier; the output stays its requested size and the handle stays valid.
    viewport->SetViewState({
        .World = scene.get(),
        .Camera = FrontCamera(initialExtent),
        .Delta = 0.0f,
    });
    Context.ImmediateCommands([&](CommandBuffer& cmd) { viewport->Render(cmd); });

    REQUIRE(viewport->GetOutput() != nullptr);
    CHECK(viewport->GetOutput()->GetImage()->GetWidth() == initialExtent.x);
    CHECK(viewport->GetOutput()->GetImage()->GetHeight() == initialExtent.y);
    CHECK(viewport->GetOutputHandle().IsValid());

    // A region extent change debounces an internal resize to the next Render: the
    // output is recreated at the new extent and the handle re-registered (so the
    // prior output Ref and handle index both change).
    constexpr uvec2 resizedExtent{96, 32};
    const Ref<ImageView> beforeResize = viewport->GetOutput();
    const u32 handleBefore = viewport->GetOutputHandle().Index;

    viewport->SetRegion({.Offset = {0, 0}, .Extent = resizedExtent});
    // The offset is stored immediately; the extent applies on the next Render.
    CHECK(viewport->GetRegion().Offset == ivec2{0, 0});

    viewport->SetViewState({
        .World = scene.get(),
        .Camera = FrontCamera(resizedExtent),
        .Delta = 0.0f,
    });
    Context.ImmediateCommands([&](CommandBuffer& cmd) { viewport->Render(cmd); });

    const Ref<ImageView> afterResize = viewport->GetOutput();
    REQUIRE(afterResize != nullptr);
    CHECK(afterResize.get() != beforeResize.get());
    CHECK(afterResize->GetImage()->GetWidth() == resizedExtent.x);
    CHECK(afterResize->GetImage()->GetHeight() == resizedExtent.y);
    CHECK(viewport->GetOutputHandle().IsValid());
    CHECK(viewport->GetOutputHandle().Index != handleBefore);

    // A zero extent is ignored: it neither changes the region nor drives a resize.
    viewport->SetRegion({.Offset = {0, 0}, .Extent = {0, 0}});
    CHECK(viewport->GetRegion().Extent == resizedExtent);
    Context.ImmediateCommands([&](CommandBuffer& cmd) { viewport->Render(cmd); });
    CHECK(viewport->GetOutput()->GetImage()->GetWidth() == resizedExtent.x);
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "viewport: with dynamic resolution off the static render scale sizes the allocation; a change "
    "resizes; a supersample grows it")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    constexpr uvec2 region{64, 48};
    const Unique<Scene> scene = Scene::Create(Types);
    const Ref<Mesh> cube = PopulateCubeScene(Context, assets, *scene);

    // Dynamic resolution off: the static scale is the allocation ceiling, so a half-scale sizes the
    // target to region/2 directly — not a full-region allocation rendered into a sub-rect.
    const Unique<Viewport> viewport = Viewport::Create({
        .Context = Context,
        .Assets = assets,
        .Region = {.Offset = {0, 0}, .Extent = region},
        .RenderScale = 0.5f,
        .Role = ViewportRole::Offscreen,
    });
    CHECK(viewport->GetRegion().Extent == region);
    CHECK(viewport->GetRenderScale() == doctest::Approx(0.5f));
    CHECK(viewport->GetOutput()->GetImage()->GetWidth() == region.x / 2);
    CHECK(viewport->GetOutput()->GetImage()->GetHeight() == region.y / 2);

    viewport->SetViewState({.World = scene.get(), .Camera = FrontCamera(region), .Delta = 0.0f});
    Context.ImmediateCommands([&](CommandBuffer& cmd) { viewport->Render(cmd); });
    // The allocation is rendered fully (the static scale is the ceiling, so the fraction is 1).
    CHECK(viewport->GetRenderer().GetValidExtent() == uvec2{32, 24});

    // Changing the static scale moves the allocation ceiling: a real resize (generation bumps, a
    // fresh output Ref) — the deliberate cost of not over-allocating a scale that never grows.
    const Ref<ImageView> beforeScale = viewport->GetOutput();
    const u64 generationBefore = viewport->GetOutputGeneration();
    viewport->SetRenderScale(1.0f);
    Context.ImmediateCommands([&](CommandBuffer& cmd) { viewport->Render(cmd); });
    CHECK(viewport->GetOutputGeneration() > generationBefore);
    CHECK(viewport->GetOutput().get() != beforeScale.get());
    CHECK(viewport->GetOutput()->GetImage()->GetWidth() == region.x);
    CHECK(viewport->GetRenderer().GetValidExtent() == region);

    // A supersample (scale > 1) grows the allocation, rendered fully (valid == allocation).
    viewport->SetRenderScale(2.0f);
    Context.ImmediateCommands([&](CommandBuffer& cmd) { viewport->Render(cmd); });
    CHECK(viewport->GetOutput()->GetImage()->GetWidth() == region.x * 2);
    CHECK(viewport->GetOutput()->GetImage()->GetHeight() == region.y * 2);
    CHECK(viewport->GetRenderer().GetValidExtent() == uvec2{region.x * 2, region.y * 2});
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "viewport: MaxAllocationScale caps the allocation below the region; resize re-applies the cap")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    // A 2× HiDPI backing extent: the region carries the full backing pixels, and the cap brings the
    // allocation back to logical-point resolution (half each axis).
    constexpr uvec2 backing{128, 96};
    const Unique<Scene> scene = Scene::Create(Types);
    const Ref<Mesh> cube = PopulateCubeScene(Context, assets, *scene);

    const Unique<Viewport> viewport = Viewport::Create({
        .Context = Context,
        .Assets = assets,
        .Region = {.Offset = {0, 0}, .Extent = backing},
        .MaxAllocationScale = 0.5f,
        .Role = ViewportRole::Offscreen,
    });

    // The region is the full backing extent, but the allocation (GetOutput()) is the capped size.
    CHECK(viewport->GetRegion().Extent == backing);
    CHECK(viewport->GetOutput()->GetImage()->GetWidth() == backing.x / 2);
    CHECK(viewport->GetOutput()->GetImage()->GetHeight() == backing.y / 2);

    viewport->SetViewState({.World = scene.get(), .Camera = FrontCamera(backing), .Delta = 0.0f});
    Context.ImmediateCommands([&](CommandBuffer& cmd) { viewport->Render(cmd); });
    CHECK(viewport->GetRenderer().GetValidExtent() == uvec2{64, 48});

    // A resize keeps feeding the full backing extent as the region; the cap re-applies, so the new
    // allocation is the capped size of the new backing extent (resize tracking honors the cap).
    constexpr uvec2 resizedBacking{96, 64};
    viewport->SetRegion({.Offset = {0, 0}, .Extent = resizedBacking});
    viewport->SetViewState(
        {.World = scene.get(), .Camera = FrontCamera(resizedBacking), .Delta = 0.0f});
    Context.ImmediateCommands([&](CommandBuffer& cmd) { viewport->Render(cmd); });
    CHECK(viewport->GetRegion().Extent == resizedBacking);
    CHECK(viewport->GetOutput()->GetImage()->GetWidth() == resizedBacking.x / 2);
    CHECK(viewport->GetOutput()->GetImage()->GetHeight() == resizedBacking.y / 2);
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "viewport: dynamic resolution sizes the allocation to MaxScale; the current scale sub-rects it "
    "without resizing; a MaxScale change resizes")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    constexpr uvec2 region{64, 48};
    const Unique<Scene> scene = Scene::Create(Types);
    const Ref<Mesh> cube = PopulateCubeScene(Context, assets, *scene);

    const Unique<Viewport> viewport = Viewport::Create({
        .Context = Context,
        .Assets = assets,
        .Region = {.Offset = {0, 0}, .Extent = region},
        .Role = ViewportRole::Offscreen,
    });
    viewport->SetViewState({.World = scene.get(), .Camera = FrontCamera(region), .Delta = 0.0f});
    Context.ImmediateCommands([&](CommandBuffer& cmd) { viewport->Render(cmd); });
    CHECK(viewport->GetOutput()->GetImage()->GetWidth() == region.x);

    // Enabling dynamic resolution with a sub-1 MaxScale shrinks the allocation to region * MaxScale:
    // a real resize, and the current scale is clamped down into [MinScale, MaxScale]. (No frame loop,
    // so GetLastGpuFrameTimeMs stays 0 and the controller holds the scale each Render.)
    const u64 generationBefore = viewport->GetOutputGeneration();
    viewport->SetDynamicResolution({.MinScale = 0.5f, .MaxScale = 0.75f});
    CHECK(viewport->IsDynamicResolutionEnabled());
    CHECK(viewport->GetRenderScale() == doctest::Approx(0.75f));
    Context.ImmediateCommands([&](CommandBuffer& cmd) { viewport->Render(cmd); });
    CHECK(viewport->GetOutputGeneration() > generationBefore);
    CHECK(viewport->GetOutput()->GetImage()->GetWidth() == 48);
    CHECK(viewport->GetOutput()->GetImage()->GetHeight() == 36);
    // At the ceiling the allocation renders fully.
    CHECK(viewport->GetRenderer().GetValidExtent() == uvec2{48, 36});

    // A current-scale move below MaxScale rides the per-frame sub-rect: no resize, stable output Ref
    // — the no-hitch dynamic-resolution win.
    const Ref<ImageView> beforeSubRect = viewport->GetOutput();
    const u64 generationSubRect = viewport->GetOutputGeneration();
    viewport->SetRenderScale(0.5f);
    Context.ImmediateCommands([&](CommandBuffer& cmd) { viewport->Render(cmd); });
    CHECK(viewport->GetOutputGeneration() == generationSubRect);
    CHECK(viewport->GetOutput().get() == beforeSubRect.get());
    CHECK(viewport->GetOutput()->GetImage()->GetWidth() == 48);
    // round(allocation {48,36} * (0.5 / 0.75)).
    CHECK(viewport->GetRenderer().GetValidExtent() == uvec2{32, 24});

    // Raising MaxScale resizes the allocation back up (the point of the option).
    const u64 generationMax = viewport->GetOutputGeneration();
    viewport->SetDynamicResolution({.MinScale = 0.5f, .MaxScale = 1.0f});
    Context.ImmediateCommands([&](CommandBuffer& cmd) { viewport->Render(cmd); });
    CHECK(viewport->GetOutputGeneration() > generationMax);
    CHECK(viewport->GetOutput()->GetImage()->GetWidth() == region.x);
    CHECK(viewport->GetOutput()->GetImage()->GetHeight() == region.y);

    // Clearing reverts the allocation ceiling to the held static scale (0.5), resizing down.
    CHECK(viewport->GetRenderScale() == doctest::Approx(0.5f));
    const u64 generationClear = viewport->GetOutputGeneration();
    viewport->ClearDynamicResolution();
    CHECK_FALSE(viewport->IsDynamicResolutionEnabled());
    Context.ImmediateCommands([&](CommandBuffer& cmd) { viewport->Render(cmd); });
    CHECK(viewport->GetOutputGeneration() > generationClear);
    CHECK(viewport->GetOutput()->GetImage()->GetWidth() == region.x / 2);
    CHECK(viewport->GetOutput()->GetImage()->GetHeight() == region.y / 2);
    CHECK(viewport->GetRenderer().GetValidExtent() == uvec2{32, 24});
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "viewport: the allocation-tier controller steps the allocation down under sustained low scale "
    "and back up on recovery; a one-frame spike does not move it")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    constexpr uvec2 region{64, 48};
    const Unique<Scene> scene = Scene::Create(Types);
    const Ref<Mesh> cube = PopulateCubeScene(Context, assets, *scene);

    const Unique<Viewport> viewport = Viewport::Create({
        .Context = Context,
        .Assets = assets,
        .Region = {.Offset = {0, 0}, .Extent = region},
        .Role = ViewportRole::Offscreen,
    });

    // Enable both loops with the default tiers {1.0, 0.75, 0.5}. The headless device has no GPU
    // timing, so the inner loop is inert (UpdateDynamicResolution no-ops); the test feeds the
    // sub-rect scale directly through SetRenderScale (the manual/override path), and the outer
    // loop steps the tier off that. The baseline tier (index 0) allocates at the full region.
    viewport->SetDynamicResolution({.MinScale = 0.25f, .MaxScale = 1.0f}, AllocationTierSettings{});
    CHECK(viewport->GetAllocationTierIndex() == 0u);
    CHECK(viewport->GetAllocationScale() == doctest::Approx(1.0f));

    // Drives one frame at the given sub-rect scale and frame delta: SetRenderScale stands in for
    // the inner loop's output (timing is unsupported here), and the delta feeds the outer loop's
    // EMA + dwell timers.
    const auto driveFrame = [&](f32 renderScale, f32 delta)
    {
        viewport->SetRenderScale(renderScale);
        viewport->SetViewState(
            {.World = scene.get(), .Camera = FrontCamera(region), .Delta = delta});
        Context.ImmediateCommands([&](CommandBuffer& cmd) { viewport->Render(cmd); });
    };

    // A single low-scale spike inside an otherwise full-scale trace must not move the tier: the
    // EMA barely dips and the dwell never accumulates. Run a steady stretch, inject one spike.
    for (u32 i = 0; i < 8; ++i)
    {
        driveFrame(1.0f, 0.25f);
    }
    driveFrame(0.25f, 0.25f);
    for (u32 i = 0; i < 8; ++i)
    {
        driveFrame(1.0f, 0.25f);
    }
    CHECK(viewport->GetAllocationTierIndex() == 0u);
    CHECK(viewport->GetOutput()->GetImage()->GetWidth() == region.x);
    CHECK(viewport->GetOutput()->GetImage()->GetHeight() == region.y);

    // A sustained sub-rect at 0.6 drives the EMA below the tier-1 step-down threshold (0.75 + the
    // margin) while still fitting tier 1; after the shrink dwell the tier steps down once and the
    // SceneRenderer allocation shrinks to round(region * 0.75) — the visually-continuous reclaim.
    // 0.6 sits above tier 2's scale (0.5), so the instantaneous-fit guard holds the controller at
    // tier 1 rather than stepping a second time.
    const u64 generationBefore = viewport->GetOutputGeneration();
    for (u32 i = 0; i < 40; ++i)
    {
        driveFrame(0.6f, 0.25f);
    }
    CHECK(viewport->GetAllocationTierIndex() == 1u);
    CHECK(viewport->GetAllocationScale() == doctest::Approx(0.75f));
    CHECK(viewport->GetOutputGeneration() > generationBefore);
    CHECK(viewport->GetOutput()->GetImage()->GetWidth() == 48);
    CHECK(viewport->GetOutput()->GetImage()->GetHeight() == 36);

    // A sustained recovery (full sub-rect) drives the EMA back above the up-hysteresis threshold;
    // after the longer grow dwell the tier steps back up and the allocation grows to the baseline.
    const u64 generationGrow = viewport->GetOutputGeneration();
    for (u32 i = 0; i < 80; ++i)
    {
        driveFrame(1.0f, 0.25f);
    }
    CHECK(viewport->GetAllocationTierIndex() == 0u);
    CHECK(viewport->GetAllocationScale() == doctest::Approx(1.0f));
    CHECK(viewport->GetOutputGeneration() > generationGrow);
    CHECK(viewport->GetOutput()->GetImage()->GetWidth() == region.x);
    CHECK(viewport->GetOutput()->GetImage()->GetHeight() == region.y);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "viewport: GPU frame timing reports a measurement and adaptive resolution stays "
                  "within its bounds")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    constexpr uvec2 region{64, 48};
    const Unique<Scene> scene = Scene::Create(Types);
    const Ref<Mesh> cube = PopulateCubeScene(Context, assets, *scene);

    const Unique<Viewport> viewport = Viewport::Create({
        .Context = Context,
        .Assets = assets,
        .Region = {.Offset = {0, 0}, .Extent = region},
        .Role = ViewportRole::Offscreen,
    });
    viewport->SetViewState({.World = scene.get(), .Camera = FrontCamera(region), .Delta = 0.0f});

    const auto driveFrame = [&]
    {
        CommandBuffer& cmd = Context.BeginFrame();
        viewport->Render(cmd);
        Context.EndFrame();
        Context.WaitIdle();
    };

    // Drive enough frames that each in-flight slot's timestamp pair has been written and read back.
    for (u32 i = 0; i < 6; ++i)
    {
        driveFrame();
    }

    // The reported time is never negative; on a device with timestamp support it is positive after
    // real GPU work. A device without support reports zero (IsGpuTimingSupported() is false).
    CHECK(Context.GetLastGpuFrameTimeMs() >= 0.0f);
    if (Context.IsGpuTimingSupported())
    {
        CHECK(Context.GetLastGpuFrameTimeMs() > 0.0f);
    }

    // With adaptive resolution on, further frames keep the scale inside [MinScale, MaxScale].
    viewport->SetDynamicResolution({
        .TargetFrameTimeMs = 1000.0f / 60.0f,
        .MinScale = 0.5f,
        .MaxScale = 1.0f,
    });
    for (u32 i = 0; i < 8; ++i)
    {
        driveFrame();
    }
    CHECK(viewport->GetRenderScale() >= 0.5f);
    CHECK(viewport->GetRenderScale() <= 1.0f);
}
