// The post-resolve translucent layer on a real device. A material declaring "placement":
// "postResolve" is drawn by a translucent pass at the HDR tail, at the output resolution through the
// unjittered projection and with no depth test — so a surface standing behind an opaque cube is
// drawn over it. Its first frame folds into the scene's translucent pass (the transition frame,
// depth-tested), every later one draws in the tail pass alone, at full and at reduced render scale.

#include <filesystem>
#include <vector>

#include <glm/gtc/packing.hpp>

#include <doctest/doctest.h>

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
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>
#include "support/TempPath.h"
#include "support/TestCook.h"

using namespace Veng;
using namespace Veng::Renderer;

#ifdef GPU_GBUFFER_FIXTURE_DIR

namespace
{
    constexpr uvec2 Extent{64, 64};

    vec3 RgbAt(const std::vector<u8>& rgba16f, const u32 x, const u32 y)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        const usize base = (static_cast<usize>(y) * Extent.x + x) * 4;
        return vec3(glm::unpackHalf1x16(halves[base + 0]), glm::unpackHalf1x16(halves[base + 1]),
                    glm::unpackHalf1x16(halves[base + 2]));
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "post-resolve translucent: a post-resolve material draws in the tail pass, over "
                  "the opaque scene, at any render scale")
{
    RegisterBuiltinTypes(Types);

    const path outArchive =
        Veng::TestSupport::TempDir() / "veng_gpu_post_resolve_translucent.vengpack";
    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(Veng::TestSupport::CookCached(
                cooker, path(GPU_GBUFFER_FIXTURE_DIR) / "translucent_pack.json", outArchive, {},
                nullptr, nullptr, nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR))
                .has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());
    const AssetResult<AssetHandle<MaterialInstance>> brick =
        assets.LoadSync<MaterialInstance>(AssetId{0x895443});
    REQUIRE(brick.has_value());
    const AssetResult<AssetHandle<MaterialInstance>> hologram =
        assets.LoadSync<MaterialInstance>(AssetId{0x83FB4D64EC6D21DCULL});
    REQUIRE(hologram.has_value());
    CHECK(hologram->Get()->GetParent()->IsPostResolve());

    // A two-unit opaque cube at the origin, and the post-resolve cube entirely behind it: seen from
    // z = +6 the opaque one covers the other completely.
    const Unique<Scene> scene = Scene::Create(Types);
    const Entity opaque = scene->CreateEntity();
    scene->Add<Transform>(opaque);
    scene->Add<MeshRenderer>(opaque).Mesh =
        assets.Adopt(Mesh::BuildSync(Context, Primitives::Cube(2.0f, *brick), "Opaque Cube"));
    const Entity behind = scene->CreateEntity();
    scene->Add<Transform>(behind).Position = vec3(0.0f, 0.0f, -2.5f);
    scene->Add<MeshRenderer>(behind).Mesh = assets.Adopt(
        Mesh::BuildSync(Context, Primitives::Cube(1.0f, *hologram), "Post-Resolve Cube"));

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = Extent,
        .Settings = {.Bloom = false, .Shadows = false, .AO = false},
    });
    CameraView camera;
    camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 0.0f, 6.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

    const auto Render = [&](const f32 scale)
    {
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer->Execute(cmd, Veng::Renderer::SceneView{.World = *scene,
                                                                 .Camera = camera,
                                                                 .Delta = 0.0f,
                                                                 .RenderScale = scale,
                                                                 .Exposure = 1.0f,
                                                                 .Tonemapper = Tonemapper::None});
            });
        return renderer->GetOutput()->GetImage()->Download();
    };
    const u32 mid = Extent.x / 2;

    // The activation frame draws the surface in the scene's pass, where the opaque cube hides it.
    const vec3 first = RgbAt(Render(1.0f), mid, mid);
    CHECK_FALSE(renderer->DidRecordPassLastFrame("Post-Resolve Translucent"));
    CHECK(first.y < 0.5f);

    for (const f32 scale : {1.0f, 0.6f})
    {
        const vec3 center = RgbAt(Render(scale), mid, mid);
        CHECK(renderer->DidRecordPassLastFrame("Post-Resolve Translucent"));
        CHECK(center.x == doctest::Approx(0.0f).epsilon(0.03));
        CHECK(center.y == doctest::Approx(1.0f).epsilon(0.03));
        CHECK(center.z == doctest::Approx(0.0f).epsilon(0.03));
    }
}

#endif
