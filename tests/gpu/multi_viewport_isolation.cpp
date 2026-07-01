// Multi-viewport isolation: two Offscreen viewports rendering the SAME scene through
// DIFFERENT cameras in one frame must each reflect their own camera. The view-constants
// (and light) buffer is shared across every viewport on the Context; if it rings only by
// frame-in-flight, the second viewport's Execute overwrites the region the first's draws
// still read at submit, so both render through the last camera — a material preview bleeding
// into the level viewport. BindlessRegistry::BeginView gives each render its own region;
// this asserts the near viewport shows the cube (its camera) while the away viewport (looking
// away, registered last) shows background, which the shared-region bug would flip.

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
    // One RGBA16F output texel decoded to a linear vec3 (the viewports request RGBA16Sfloat).
    vec3 DecodeTexel(const vector<u8>& rgba16f, u32 width, u32 x, u32 y)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        const usize base = (static_cast<usize>(y) * width + x) * 4;
        return vec3(glm::unpackHalf1x16(halves[base + 0]), glm::unpackHalf1x16(halves[base + 1]),
                    glm::unpackHalf1x16(halves[base + 2]));
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "multi-viewport isolation: two viewports with different cameras in one frame "
                  "each render through their own view, not the last one registered")
{
    RegisterBuiltinTypes(Types);

    // Cook the brick g-buffer fixture in-process (a single material is enough; both viewports
    // draw the same green-tinted cube, differing only by camera).
    const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
    const path tempDir = std::filesystem::temp_directory_path();
    const path packJson = tempDir / "veng_gpu_multi_viewport_pack.json";
    const path outArchive = tempDir / "veng_gpu_multi_viewport.vengpack";

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
            << (fixtureDir / "materials/brick.vmat.json").string() << R"(" }
  ]
})";
    }

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker
                .CookPack(packJson, outArchive, {}, nullptr, nullptr, nullptr, nullptr, {},
                          path(VENG_CORE_SHADER_DIR))
                .has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    constexpr uvec2 extent{96, 96};

    // A green-tinted brick cube at the origin, shared by both viewports.
    const AssetResult<AssetHandle<MaterialInstance>> material =
        assets.LoadSync<MaterialInstance>(AssetId{9000003}); // brick default instance (over 9003)
    REQUIRE(material.has_value());
    REQUIRE(material->IsLoaded());
    const_cast<MaterialInstance&>(*material->Get())
        .SetParam("BaseColorFactor", vec4(0.0f, 1.0f, 0.0f, 1.0f));

    const Ref<Mesh> cubeMesh = Mesh::BuildSync(Context, Primitives::Cube(1.4f, *material), "Cube");

    const Unique<Scene> scene = Scene::Create(Types);
    {
        const Entity cube = scene->CreateEntity();
        scene->Add<Transform>(cube);
        scene->Add<MeshRenderer>(cube).Mesh = assets.Adopt(cubeMesh);
    }

    const f32 aspect = static_cast<f32>(extent.x) / static_cast<f32>(extent.y);

    // The "near" camera looks at the origin, so the cube fills the frame center.
    CameraView nearCamera;
    nearCamera.SetPerspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);
    nearCamera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

    // The "away" camera sits at the same eye but looks in +Z, away from the cube — its center
    // is cleared background. It is registered LAST, so a shared per-frame view region would hold
    // its camera at submit and drag the near viewport's draws onto it (the bleed under test).
    CameraView awayCamera;
    awayCamera.SetPerspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);
    awayCamera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f, 0.0f, 6.0f), vec3(0.0f, 1.0f, 0.0f));

    const Unique<Viewport> nearView = Viewport::Create({
        .Context = Context,
        .Assets = assets,
        .Region = {.Offset = {0, 0}, .Extent = extent},
        .ColorFormat = Format::RGBA16Sfloat,
        .Settings = {.Mode = DebugView::Albedo},
        .Role = ViewportRole::Offscreen,
    });
    nearView->SetViewState({.World = scene.get(), .Camera = nearCamera, .Delta = 0.0f});

    const Unique<Viewport> awayView = Viewport::Create({
        .Context = Context,
        .Assets = assets,
        .Region = {.Offset = {0, 0}, .Extent = extent},
        .ColorFormat = Format::RGBA16Sfloat,
        .Settings = {.Mode = DebugView::Albedo},
        .Role = ViewportRole::Offscreen,
    });
    awayView->SetViewState({.World = scene.get(), .Camera = awayCamera, .Delta = 0.0f});

    // Drive-list order = render order: the near viewport first, the away viewport last.
    vector<Viewport*> driveList;
    driveList.emplace_back(nearView.get());
    nearView->AttachToDriveList(driveList);
    driveList.emplace_back(awayView.get());
    awayView->AttachToDriveList(driveList);

    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            for (Viewport* viewport : driveList)
            {
                viewport->Render(cmd);
            }
        });

    // The near viewport's center is the green cube — this is what fails under the shared-region
    // bug (it would render through the away camera and show cleared background instead).
    const vector<u8> nearPixels = nearView->GetOutput()->GetImage()->Download();
    REQUIRE(nearPixels.size() == static_cast<size_t>(extent.x) * extent.y * 8);
    const vec3 nearCenter = DecodeTexel(nearPixels, extent.x, extent.x / 2, extent.y / 2);
    CHECK(nearCenter.g > 0.1f);
    CHECK(nearCenter.g > nearCenter.r);
    CHECK(nearCenter.g > nearCenter.b);

    // The away viewport's center is cleared background (the cube is behind its camera): its green
    // is far below the near viewport's, proving the two rendered through distinct view regions.
    const vector<u8> awayPixels = awayView->GetOutput()->GetImage()->Download();
    REQUIRE(awayPixels.size() == static_cast<size_t>(extent.x) * extent.y * 8);
    const vec3 awayCenter = DecodeTexel(awayPixels, extent.x, extent.x / 2, extent.y / 2);
    CHECK(awayCenter.g < 0.05f);
    CHECK(nearCenter.g > awayCenter.g + 0.1f);

    std::filesystem::remove(outArchive);
    std::filesystem::remove(packJson);
}

#endif // GPU_GBUFFER_FIXTURE_DIR
