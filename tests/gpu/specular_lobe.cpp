// The GGX lobe is evaluated in full, and the lighting output is guarded against the half-float
// overflow a full-strength lobe can reach. A flat white metal plane under a directional light is
// framed on the light's mirror reflection, through a narrow field of view whose texels are several
// times finer than the narrowest lobe's half-maximum width — a plane rather than a sphere, so a
// texel's angle on screen is its angle in reflection and the brightest texel sits on the peak. The
// lit HDR scene colour is read before tonemap, since the tonemapped output clamps to [0, 1] and can
// show neither the peak nor an overflow.

#include <algorithm>
#include <array>
#include <cmath>

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
#include <Veng/Renderer/LightPacking.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>

#include "support/TempPath.h"
#include "support/TestCook.h"

// glm's packing header after Veng.h, which configures glm first.
#include <glm/gtc/packing.hpp>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // Mirrors Veng/lighting.slang's LightingOutputMax: the per-channel bound on lit radiance.
    constexpr f32 LightingOutputMax = 6.0e4f;

    // The white-plane fixture pack's material: the g-buffer brick material over a white texture,
    // writing RoughnessFactor and MetallicFactor straight into the g-buffer.
    constexpr AssetId WhitePlaneInstanceId{0x895443ULL};

    constexpr uvec2 Extent{128, 128};

    // The largest finite channel value across the lit HDR target, and whether every texel is finite.
    struct HdrScan
    {
        f32 Max = 0.0f;
        bool Finite = true;
    };

    HdrScan ScanHdr(const vector<u8>& rgba16f)
    {
        HdrScan scan;
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        for (usize i = 0; i < rgba16f.size() / sizeof(u16); i += 4)
        {
            for (usize channel = 0; channel < 3; ++channel)
            {
                const f32 value = glm::unpackHalf1x16(halves[i + channel]);
                scan.Finite = scan.Finite && std::isfinite(value);
                scan.Max = std::isfinite(value) ? std::max(scan.Max, value) : scan.Max;
            }
        }
        return scan;
    }

    // A metal plane at the origin, a directional light 45 degrees up behind it, and a camera on the
    // light's mirror reflection looking at the plane's centre through a 4-degree field of view: a
    // texel spans about 0.03 degrees in reflection, against a half-maximum lobe radius of about 0.12
    // degrees at the roughness floor.
    struct MirrorScene
    {
        Unique<Scene> World;
        Unique<SceneRenderer> Renderer;
        CameraView Camera;
        Entity Sun;
    };

    MirrorScene MakeMirrorScene(Context& context, AssetManager& assets, TypeRegistry& types,
                                const AssetHandle<MaterialInstance>& material)
    {
        MirrorScene mirror;
        mirror.World = Scene::Create(types);

        const Ref<Mesh> plane = Mesh::BuildSync(
            context, Primitives::Plane(vec2(8.0f), uvec2(1), material), "Mirror Plane");
        const Entity ground = mirror.World->CreateEntity();
        mirror.World->Add<Transform>(ground);
        mirror.World->Add<MeshRenderer>(ground).Mesh = assets.Adopt(plane);

        const vec3 toLight = glm::normalize(vec3(0.0f, 1.0f, 1.0f));
        const vec3 toCamera = glm::normalize(vec3(0.0f, 1.0f, -1.0f));
        mirror.Sun = mirror.World->CreateEntity();
        mirror.World->Add<Light>(mirror.Sun) = Light{
            .Type = LightType::Directional,
            .Direction = -toLight,
            .Color = vec3(1.0f),
        };

        mirror.Camera.SetPerspective(glm::radians(4.0f), 1.0f, 0.1f, 100.0f);
        mirror.Camera.SetView(toCamera * 4.0f, vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

        mirror.Renderer = SceneRenderer::Create({
            .Context = context,
            .Assets = assets,
            .OutputFormat = context.GetOutputFormat(),
            .Extent = Extent,
            .Settings = {.Mode = DebugView::Final, .Bloom = false, .Shadows = false, .AO = false},
        });
        return mirror;
    }

    // Sets the sun's radiance and the plane's roughness, renders, and scans the lit HDR target.
    HdrScan RenderMirror(const Context& context, MirrorScene& mirror, MaterialInstance& material,
                         f32 radiance, f32 roughness)
    {
        mirror.World->Get<Light>(mirror.Sun).Intensity = radiance / LuminousAnchor;
        material.SetParam("RoughnessFactor", roughness);
        context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                mirror.Renderer->Execute(cmd, Renderer::SceneView{.World = *mirror.World,
                                                                  .Camera = mirror.Camera,
                                                                  .Delta = 0.0f});
            });
        const Ref<ImageView> hdr = mirror.Renderer->GetHdrView();
        REQUIRE(hdr != nullptr);
        return ScanHdr(hdr->GetImage()->Download());
    }

    AssetHandle<MaterialInstance> LoadWhiteMetal(AssetManager& assets, const path& archive)
    {
        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        const VoidResult cookResult = Veng::TestSupport::CookCached(
            cooker, path(GPU_GBUFFER_FIXTURE_DIR) / "white_plane_pack.json", archive, {}, nullptr,
            nullptr, nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR));
        REQUIRE(cookResult.has_value());
        REQUIRE(assets.Mount(archive).has_value());

        const AssetResult<AssetHandle<MaterialInstance>> material =
            assets.LoadSync<MaterialInstance>(WhitePlaneInstanceId);
        REQUIRE(material.has_value());
        REQUIRE(material->IsLoaded());
        const_cast<MaterialInstance&>(*material->Get()).SetParam("MetallicFactor", 1.0f);
        return *material;
    }
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "specular lobe: a mirror highlight's peak rises all the way to the roughness floor")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    const path archive = Veng::TestSupport::TempDir() / "veng_gpu_specular_peak.vengpack";
    const AssetHandle<MaterialInstance> material = LoadWhiteMetal(assets, archive);
    MirrorScene mirror = MakeMirrorScene(Context, assets, Types, material);
    auto& instance = const_cast<MaterialInstance&>(*material.Get());

    // A GGX peak is 1 / (pi alpha^2), so a smoother surface concentrates the same reflected energy
    // into a brighter, tighter highlight at every roughness down to the floor (0.04). The light is at
    // solar radiance, where the floor's peak still sits well under the output guard.
    constexpr std::array<f32, 4> roughnesses{0.2f, 0.1f, 0.06f, 0.04f};
    std::array<HdrScan, roughnesses.size()> scans{};
    for (usize i = 0; i < roughnesses.size(); ++i)
    {
        scans[i] = RenderMirror(Context, mirror, instance, 1.0f, roughnesses[i]);
    }

    for (usize i = 0; i < scans.size(); ++i)
    {
        CHECK(scans[i].Finite);
    }
    for (usize i = 1; i < scans.size(); ++i)
    {
        CHECK(scans[i].Max > scans[i - 1].Max);
    }
    CHECK(scans.back().Max < LightingOutputMax);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "specular lobe: a floor-roughness mirror under a very bright light stays finite "
                  "and within the output guard")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    const path archive = Veng::TestSupport::TempDir() / "veng_gpu_specular_guard.vengpack";
    const AssetHandle<MaterialInstance> material = LoadWhiteMetal(assets, archive);
    MirrorScene mirror = MakeMirrorScene(Context, assets, Types, material);
    auto& instance = const_cast<MaterialInstance&>(*material.Get());

    // A hundred times solar radiance puts the floor's peak two orders of magnitude past what a half
    // float holds; the guard holds every texel finite and at or under its bound, and the peak does
    // reach the bound, so the case exercises the guard rather than a lobe that never needed it.
    const HdrScan scan = RenderMirror(Context, mirror, instance, 100.0f, 0.04f);
    CHECK(scan.Finite);
    CHECK(scan.Max <= LightingOutputMax);
    CHECK(scan.Max > 0.5f * LightingOutputMax);

    std::filesystem::remove(archive);
}
