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
