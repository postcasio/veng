// Viewport-feeds-viewport: the render-to-texture-into-a-material proof. A producer
// Offscreen viewport renders scene A; a consumer viewport renders scene B whose
// material binds the producer's GetOutputHandle() via Material::SetTextureHandle.
// Both viewports are appended to one drive-list (producer first), then Render is
// driven for each in registration order within a single command buffer — the engine
// render phase, where registration order is render order. The consumer's output must
// reflect the producer's content, proving the producer rendered first and its output
// was sampleable when the consumer ran. The handoff records on the single graphics
// queue in submission order with no ring and no semaphore, so it runs under the
// validation gate to confirm no barrier/layout error crosses the boundary.

#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>

#include <glm/gtc/packing.hpp>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
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

#ifdef GPU_GBUFFER_FIXTURE_DIR

namespace
{
    // One RGBA16F output texel decoded to a linear vec3 — the viewport output is
    // RGBA16Sfloat (the viewports below request that format explicitly).
    vec3 DecodeTexel(const vector<u8>& rgba16f, u32 width, u32 x, u32 y)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        const usize base = (static_cast<usize>(y) * width + x) * 4;
        return vec3(glm::unpackHalf1x16(halves[base + 0]), glm::unpackHalf1x16(halves[base + 1]),
                    glm::unpackHalf1x16(halves[base + 2]));
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

// The RTT-into-material handoff. The producer renders a brick cube tinted green
// (BaseColorFactor), so its Albedo output is a distinctively green-dominant frame.
// That output is bound as the consumer cube's BaseColor through
// Material::SetTextureHandle, and the consumer renders in Albedo too, so its g-buffer
// color is exactly the producer texel × the consumer's (white) BaseColorFactor.
// Registering the producer first and driving the drive-list in order makes the
// producer's Sample transition reach the consumer's read without a ring or cross-queue
// semaphore — the consumer center reads green-dominant, which the consumer's own
// red-dominant brick (its untinted BaseColorFactor) never would, so the producer's
// output is what colored it.
TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "viewport feeds viewport: a producer's output, sampled through a consumer's "
                  "material, reflects the producer's content in registration order")
{
    RegisterBuiltinTypes(Types);

    // Cook the brick g-buffer fixture in-process. A small generated pack adds a second
    // brick material (a distinct id) reusing the same fixture sources, so the producer
    // and consumer hold independent Material instances rather than the one cached
    // entry. No new asset is minted into the tree — the second material is a build-time
    // pack the test writes and removes.
    const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
    const path tempDir = std::filesystem::temp_directory_path();
    const path packJson = tempDir / "veng_gpu_viewport_feeds_pack.json";
    const path outArchive = tempDir / "veng_gpu_viewport_feeds.vengpack";

    {
        std::ofstream out(packJson);
        out << R"({
  "version": 1,
  "assets": [
    { "id": 9201, "type": "vertex_layout", "source": ")"
            << (fixtureDir / "layouts/canonical.vlayout.json").string() << R"(" },
    { "id": 9001, "type": "texture",  "source": ")"
            << (fixtureDir / "textures/brick.tex.json").string() << R"(" },
    { "id": 9101, "type": "shader",   "source": ")"
            << (fixtureDir / "shaders/brick.vert.shader.json").string() << R"(" },
    { "id": 9102, "type": "shader",   "source": ")"
            << (fixtureDir / "shaders/brick.frag.shader.json").string() << R"(" },
    { "id": 9003, "type": "material", "source": ")"
            << (fixtureDir / "materials/brick.vmat.json").string() << R"(" },
    { "id": 9013, "type": "material", "source": ")"
            << (fixtureDir / "materials/brick.vmat.json").string() << R"(" }
  ]
})";
    }

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    // The brick shaders `#include "Veng/material.slang"`; the engine core shader dir is on
    // the cook's Slang search path so the cross-pack include resolves.
    REQUIRE(cooker
                .CookPack(packJson, outArchive, {}, nullptr, nullptr, nullptr, nullptr, {},
                          path(VENG_CORE_SHADER_DIR))
                .has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    constexpr uvec2 extent{96, 96};

    // ── Producer scene A: a brick cube tinted green ───────────────────────────────
    const AssetResult<AssetHandle<MaterialInstance>> producerMaterial =
        assets.LoadSync<MaterialInstance>(AssetId{0x232B}); // brick (9003)
    REQUIRE(producerMaterial.has_value());
    REQUIRE(producerMaterial->IsLoaded());
    const_cast<MaterialInstance&>(*producerMaterial->Get())
        .SetParam("BaseColorFactor", vec4(0.0f, 1.0f, 0.0f, 1.0f));

    const Ref<Mesh> producerCube =
        Mesh::BuildSync(Context, Primitives::Cube(1.4f, *producerMaterial), "Producer Cube");

    const Unique<Scene> sceneA = Scene::Create(Types);
    {
        const Entity cube = sceneA->CreateEntity();
        sceneA->Add<Transform>(cube);
        sceneA->Add<MeshRenderer>(cube).Mesh = assets.Adopt(producerCube);
    }

    const Unique<Viewport> producer = Viewport::Create({
        .Context = Context,
        .Assets = assets,
        .Region = {.Offset = {0, 0}, .Extent = extent},
        .ColorFormat = Format::RGBA16Sfloat,
        .Settings = {.Mode = DebugView::Albedo},
        .Role = ViewportRole::Offscreen,
    });
    producer->SetViewState({.World = sceneA.get(), .Camera = FrontCamera(extent), .Delta = 0.0f});

    // ── Consumer scene B: a brick cube whose BaseColor is the producer's output ───
    const AssetResult<AssetHandle<MaterialInstance>> consumerMaterial =
        assets.LoadSync<MaterialInstance>(AssetId{9013}); // second brick instance
    REQUIRE(consumerMaterial.has_value());
    REQUIRE(consumerMaterial->IsLoaded());

    // The loaded handle hands out a const Material; SetTextureHandle mutates the
    // ring-buffered block in place — the const_cast the runtime-bind path uses.
    const_cast<MaterialInstance&>(*consumerMaterial->Get())
        .SetTextureHandle("BaseColor", producer->GetOutputHandle());

    const Ref<Mesh> consumerCube =
        Mesh::BuildSync(Context, Primitives::Cube(1.4f, *consumerMaterial), "Consumer Cube");

    const Unique<Scene> sceneB = Scene::Create(Types);
    {
        const Entity cube = sceneB->CreateEntity();
        sceneB->Add<Transform>(cube);
        sceneB->Add<MeshRenderer>(cube).Mesh = assets.Adopt(consumerCube);
    }

    const Unique<Viewport> consumer = Viewport::Create({
        .Context = Context,
        .Assets = assets,
        .Region = {.Offset = {0, 0}, .Extent = extent},
        .ColorFormat = Format::RGBA16Sfloat,
        .Settings = {.Mode = DebugView::Albedo},
        .Role = ViewportRole::Offscreen,
    });
    consumer->SetViewState({.World = sceneB.get(), .Camera = FrontCamera(extent), .Delta = 0.0f});

    // ── The drive-list: producer first, consumer second ──────────────────────────
    // Registration order is render order. The engine render phase walks the list and
    // calls Render on each into one command buffer; mirror it here so the producer's
    // output is in Sample layout before the consumer samples it — same frame, single
    // graphics queue, no ring, no semaphore.
    vector<Viewport*> driveList;
    driveList.emplace_back(producer.get());
    producer->AttachToDriveList(driveList);
    driveList.emplace_back(consumer.get());
    consumer->AttachToDriveList(driveList);

    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            for (Viewport* viewport : driveList)
            {
                viewport->Render(cmd);
            }
        });

    // The producer's output is green-dominant where the cube fills the frame.
    const vector<u8> producerPixels = producer->GetOutput()->GetImage()->Download();
    REQUIRE(producerPixels.size() == static_cast<size_t>(extent.x) * extent.y * 8);
    const vec3 producerCenter = DecodeTexel(producerPixels, extent.x, extent.x / 2, extent.y / 2);
    CHECK(producerCenter.g > 0.1f);
    CHECK(producerCenter.g > producerCenter.r);

    // The consumer's albedo center is the sampled producer texel × its BaseColorFactor
    // — green-dominant, proving the producer rendered first and its output fed the
    // consumer's material this same frame.
    const vector<u8> consumerPixels = consumer->GetOutput()->GetImage()->Download();
    REQUIRE(consumerPixels.size() == static_cast<size_t>(extent.x) * extent.y * 8);
    const vec3 consumerCenter = DecodeTexel(consumerPixels, extent.x, extent.x / 2, extent.y / 2);
    CHECK(consumerCenter.g > 0.05f);
    CHECK(consumerCenter.g > consumerCenter.r);
    CHECK(consumerCenter.g > consumerCenter.b);

    std::filesystem::remove(outArchive);
    std::filesystem::remove(packJson);
}

#endif // GPU_GBUFFER_FIXTURE_DIR
