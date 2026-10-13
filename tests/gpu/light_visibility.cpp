// A participating medium asks whether a scene light reaches a point. A Translucent fixture that
// includes Veng/medium_lighting.slang evaluates LightVisibility at fixed world points rather than at
// its own surface, finding its light by the tag the scene authored on it, and writes each answer to
// a cell of the screen: a point in an opaque occluder's shadow reads dark, a point beside it lit,
// and a point past every shadow map's coverage lit. The directional case puts one shadowed and one
// lit probe in the middle of each cascade's depth range, so every cascade is read, and runs once
// from the half-resolution layer (single tap) and once at full resolution (3×3 PCF). The area-light
// case reads a Sphere light's punctual tile. A last case reads FindLightByTag back against the
// index the C++ packing gave the tagged light. Two more read LightShadowMapAt at the same probes:
// each cascade's probes name that cascade's texel size and far split, and an area light's tile a
// perspective texel growing with the distance from the light.

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
    constexpr uvec2 Extent{64, 64};
    constexpr u32 Columns = 4;
    constexpr u32 Rows = 3;

    // The fixture's modes (Select.x) and filters (Select.y); see light_visibility.frag.slang.
    constexpr f32 CascadeProbes = 0.0f;
    constexpr f32 FixedProbes = 1.0f;
    constexpr f32 TagLookup = 2.0f;
    constexpr f32 CascadeMapProbes = 3.0f;
    constexpr f32 TileMapProbes = 4.0f;
    constexpr f32 SingleTap = 0.0f;
    constexpr f32 Pcf = 1.0f;

    // The tag the probed light carries.
    constexpr u32 ProbedTag = 7;

    vec3 DecodeTexel(const vector<u8>& rgba16f, u32 x, u32 y)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        const usize base = (static_cast<usize>(y) * Extent.x + x) * 4;
        return vec3(glm::unpackHalf1x16(halves[base + 0]), glm::unpackHalf1x16(halves[base + 1]),
                    glm::unpackHalf1x16(halves[base + 2]));
    }

    // The centre texel of the fixture's cell (column, row).
    vec3 Cell(const vector<u8>& pixels, u32 column, u32 row)
    {
        const u32 x = (Extent.x * (2 * column + 1)) / (2 * Columns);
        const u32 y = (Extent.y * (2 * row + 1)) / (2 * Rows);
        return DecodeTexel(pixels, x, y);
    }

    struct VisibilityFixture
    {
        Veng::Test::GpuFixture& Gpu;
        path Archive;
        Unique<AssetManager> Assets;
        AssetHandle<MaterialInstance> Opaque;
        AssetHandle<MaterialInstance> Full;
        AssetHandle<MaterialInstance> Half;
        Unique<Scene> World;
        CameraView Camera;
        Unique<SceneRenderer> Draw;

        explicit VisibilityFixture(Veng::Test::GpuFixture& gpu, const char* archiveName)
            : Gpu(gpu), Archive(Veng::TestSupport::TempDir() / archiveName)
        {
            RegisterBuiltinTypes(gpu.Types);
            Cook::Cooker cooker;
            Cook::RegisterBuiltinImporters(cooker);
            REQUIRE(Veng::TestSupport::CookCached(
                        cooker, path(GPU_GBUFFER_FIXTURE_DIR) / "light_visibility_pack.json",
                        Archive, {}, nullptr, nullptr, nullptr, nullptr, {},
                        path(VENG_CORE_SHADER_DIR))
                        .has_value());
            Assets = std::make_unique<AssetManager>(gpu.Context, gpu.Tasks, gpu.Types);
            REQUIRE(Assets->Mount(Archive).has_value());
            Opaque = Load(AssetId{0xF34FF2CE488F7B19ULL});
            Full = Load(AssetId{0xC26ABDC66BCB00A2ULL});
            Half = Load(AssetId{0x4E03BBA0B2F25583ULL});

            World = Scene::Create(gpu.Types);
            Camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 200.0f);
            Camera.SetView(vec3(0.0f, 1.0f, 10.0f), vec3(0.0f, 1.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f));

            SceneRendererSettings settings;
            settings.Mode = DebugView::Final;
            settings.Bloom = false;
            settings.Shadows = true;
            settings.AO = false;
            settings.AutoExposure = false;
            // Caps the cascades well short of the occluder's far end, so a probe past them stands
            // in a shadow no map covers.
            settings.MaxShadowDistance = 40.0f;
            Draw = SceneRenderer::Create({
                .Context = gpu.Context,
                .Assets = *Assets,
                .OutputFormat = gpu.Context.GetOutputFormat(),
                .Extent = Extent,
                .Settings = settings,
            });
        }

        ~VisibilityFixture()
        {
            Draw.reset();
            World.reset();
            Assets.reset();
            std::filesystem::remove(Archive);
        }

        VisibilityFixture(const VisibilityFixture&) = delete;
        VisibilityFixture& operator=(const VisibilityFixture&) = delete;

        AssetHandle<MaterialInstance> Load(const AssetId id)
        {
            const AssetResult<AssetHandle<MaterialInstance>> instance =
                Assets->LoadSync<MaterialInstance>(id);
            REQUIRE(instance.has_value());
            REQUIRE(instance->IsLoaded());
            return *instance;
        }

        // An opaque box caster: a unit cube scaled to size, centred at position.
        void AddBox(const vec3& position, const vec3& size)
        {
            const Ref<Mesh> mesh =
                Mesh::BuildSync(Gpu.Context, Primitives::Cube(1.0f, Opaque), "Visibility Occluder");
            const Entity box = World->CreateEntity();
            World->Add<Transform>(box) = Transform{.Position = position, .Scale = size};
            World->Add<MeshRenderer>(box).Mesh = Assets->Adopt(mesh);
        }

        // The probe: a translucent cube just in front of the camera, filling the view.
        void AddProbe(const AssetHandle<MaterialInstance>& material)
        {
            const Ref<Mesh> mesh =
                Mesh::BuildSync(Gpu.Context, Primitives::Cube(2.0f, material), "Visibility Probe");
            const Entity probe = World->CreateEntity();
            World->Add<Transform>(probe).Position = vec3(0.0f, 1.0f, 8.0f);
            World->Add<MeshRenderer>(probe).Mesh = Assets->Adopt(mesh);
        }

        vector<u8> Render()
        {
            Gpu.Context.ImmediateCommands(
                [&](CommandBuffer& cmd)
                {
                    Draw->Execute(
                        cmd, Renderer::SceneView{.World = *World, .Camera = Camera, .Delta = 0.0f});
                });
            return Draw->GetOutput()->GetImage()->Download();
        }
    };

    // A directional light straight down onto a long, narrow slab that passes above the camera's
    // axis and runs far past the shadowed range.
    void BuildDirectionalScene(VisibilityFixture& f)
    {
        const Entity sun = f.World->CreateEntity();
        f.World->Add<Light>(sun) = Light{
            .Type = LightType::Directional,
            .Direction = vec3(0.0f, -1.0f, 0.0f),
            .Intensity = 1.0f / LuminousAnchor,
            .Tag = ProbedTag,
        };
        f.AddBox(vec3(0.0f, 3.0f, -65.0f), vec3(0.2f, 0.2f, 154.0f));
    }

    // Every cascade's shadowed probe reads dark, its lit probe lit, and the probe past the last
    // cascade lit; and each column's probes select that column's cascade.
    void CheckCascadeProbes(const vector<u8>& pixels)
    {
        f32 shadowedMax = 0.0f;
        f32 litMin = 1e9f;
        f32 selectsOwnCascade = 1e9f;
        for (u32 c = 0; c < Columns; ++c)
        {
            const vec3 shadowed = Cell(pixels, c, 0);
            const vec3 lit = Cell(pixels, c, 1);
            shadowedMax = glm::max(shadowedMax, shadowed.r);
            litMin = glm::min(litMin, glm::min(lit.r, Cell(pixels, c, 2).r));
            selectsOwnCascade = glm::min(selectsOwnCascade, glm::min(shadowed.b, lit.b));
        }
        CHECK(shadowedMax < 0.05f);
        CHECK(litMin > 0.3f);
        CHECK(selectsOwnCascade > 0.3f);
    }

    void SetCascadeProbes(const AssetHandle<MaterialInstance>& material, const f32 filter)
    {
        material->SetParam("Select",
                           vec4(CascadeProbes, filter, static_cast<f32>(ProbedTag), 0.0f));
        // On the axis, under the slab; beside it, past the shadow's edge; and on the axis at a
        // depth past the capped cascades, still under the slab.
        material->SetParam("Shadowed", vec4(0.0f));
        material->SetParam("Lit", vec4(0.45f, 0.0f, 0.0f, 0.0f));
        material->SetParam("Past", vec4(0.0f, 0.0f, 0.0f, 100.0f));
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "light visibility: the half-resolution layer reads every cascade of a "
                  "directional light")
{
    VisibilityFixture f(*this, "veng_gpu_light_visibility_half.vengpack");
    BuildDirectionalScene(f);
    SetCascadeProbes(f.Half, SingleTap);
    f.AddProbe(f.Half);

    // The first frame to route a half-resolution draw wires the layer; the next draws through it.
    f.Render();
    const vector<u8> pixels = f.Render();
    REQUIRE(f.Draw->DidRecordPassLastFrame("Scene Translucent Half"));
    CheckCascadeProbes(pixels);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "light visibility: a full-resolution translucent reads every cascade through "
                  "the PCF kernel")
{
    VisibilityFixture f(*this, "veng_gpu_light_visibility_full.vengpack");
    BuildDirectionalScene(f);
    SetCascadeProbes(f.Full, Pcf);
    f.AddProbe(f.Full);

    CheckCascadeProbes(f.Render());
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "light visibility: an area light's punctual tile shadows a point beneath its "
                  "occluder")
{
    VisibilityFixture f(*this, "veng_gpu_light_visibility_area.vengpack");

    // A small Sphere light over a box standing above a floor: close enough to the scene to keep
    // its own perspective tile rather than a cascade set.
    const Entity lamp = f.World->CreateEntity();
    f.World->Add<Transform>(lamp).Position = vec3(0.0f, 8.0f, 0.0f);
    f.World->Add<Light>(lamp) = Light{
        .Type = LightType::Sphere,
        .Direction = vec3(0.0f, -1.0f, 0.0f),
        .Intensity = 1.0f / LuminousAnchor,
        .Range = 50.0f,
        .Radius = 0.25f,
        .Tag = ProbedTag,
    };
    f.AddBox(vec3(0.0f, 4.0f, 0.0f), vec3(2.0f));
    f.AddBox(vec3(0.0f, -0.1f, 0.0f), vec3(20.0f, 0.2f, 20.0f));

    f.Full->SetParam("Select", vec4(FixedProbes, SingleTap, static_cast<f32>(ProbedTag), 0.0f));
    // Under the box; off to its side, where the line to the light clears it; and above the light,
    // behind its tile's projection.
    f.Full->SetParam("Shadowed", vec4(0.0f, 1.0f, 0.0f, 0.0f));
    f.Full->SetParam("Lit", vec4(4.0f, 1.0f, 0.0f, 0.0f));
    f.Full->SetParam("Past", vec4(0.0f, 12.0f, 0.0f, 0.0f));
    f.AddProbe(f.Full);

    const vector<u8> pixels = f.Render();
    CHECK(Cell(pixels, 1, 0).r < 0.05f);
    CHECK(Cell(pixels, 1, 1).r > 0.3f);
    CHECK(Cell(pixels, 1, 2).r > 0.3f);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "light visibility: each cascade's points name that cascade's map")
{
    VisibilityFixture f(*this, "veng_gpu_light_visibility_cascade_map.vengpack");
    BuildDirectionalScene(f);
    SetCascadeProbes(f.Half, SingleTap);
    f.Half->SetParam("Select",
                     vec4(CascadeMapProbes, SingleTap, static_cast<f32>(ProbedTag), 0.0f));
    f.AddProbe(f.Half);

    f.Render();
    const vector<u8> pixels = f.Render();
    f32 named = 1e9f;
    for (u32 c = 0; c < Columns; ++c)
    {
        for (u32 row = 0; row < 2; ++row)
        {
            const vec3 cell = Cell(pixels, c, row);
            named = glm::min(named, glm::min(cell.r, glm::min(cell.g, cell.b)));
        }
        named = glm::min(named, Cell(pixels, c, 2).r);
    }
    CHECK(named > 0.3f);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "light visibility: an area light's tile resolves finer nearer the light")
{
    VisibilityFixture f(*this, "veng_gpu_light_visibility_tile_map.vengpack");
    const Entity lamp = f.World->CreateEntity();
    f.World->Add<Transform>(lamp).Position = vec3(0.0f, 8.0f, 0.0f);
    f.World->Add<Light>(lamp) = Light{
        .Type = LightType::Sphere,
        .Direction = vec3(0.0f, -1.0f, 0.0f),
        .Intensity = 1.0f / LuminousAnchor,
        .Range = 50.0f,
        .Radius = 0.25f,
        .Tag = ProbedTag,
    };
    f.AddBox(vec3(0.0f, -0.1f, 0.0f), vec3(20.0f, 0.2f, 20.0f));

    // Seven units below the light on its axis and a little to the side of it, and 6.25 below it,
    // all inside the tile's fitted depth range: a perspective texel is as wide as its depth.
    f.Full->SetParam("Select",
                     vec4(TileMapProbes, SingleTap, static_cast<f32>(ProbedTag), 6.25f / 7.0f));
    f.Full->SetParam("Shadowed", vec4(0.0f, 1.0f, 0.0f, 0.0f));
    f.Full->SetParam("Lit", vec4(0.5f, 1.0f, 0.0f, 0.0f));
    f.Full->SetParam("Past", vec4(0.0f, 1.75f, 0.0f, 0.0f));
    f.AddProbe(f.Full);

    const vec3 centre = Cell(f.Render(), 1, 1);
    CHECK(centre.r > 0.3f);
    CHECK(centre.g > 0.3f);
    CHECK(centre.b > 0.3f);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "light visibility: a shader finds the light the C++ packing tagged")
{
    VisibilityFixture f(*this, "veng_gpu_light_visibility_tag.vengpack");

    // An untagged light ahead of the tagged one in scene order, so the tagged light is not index 0.
    const Entity fill = f.World->CreateEntity();
    f.World->Add<Light>(fill) = Light{.Type = LightType::Directional, .CastsShadows = false};
    BuildDirectionalScene(f);

    const PackedSceneLights packed = PackSceneLights(*f.World, true, 1024);
    u32 tagged = packed.LightCount;
    for (u32 i = 0; i < packed.LightCount; ++i)
    {
        if (UnpackLightTag(packed.Lights[i]) == ProbedTag)
        {
            tagged = i;
        }
    }
    REQUIRE(tagged == 1);

    f.Full->SetParam("Select",
                     vec4(TagLookup, 0.0f, static_cast<f32>(ProbedTag), static_cast<f32>(tagged)));
    f.AddProbe(f.Full);

    const vec3 centre = Cell(f.Render(), 1, 1);
    CHECK(centre.r > 0.3f);
    CHECK(centre.g > 0.3f);
}
