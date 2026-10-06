// The GGX lobe is evaluated in full, a light's size widens it, specular anti-aliasing widens it by
// how far the normal varies across a pixel, and the lighting output is guarded against the
// half-float overflow a full-strength lobe can reach. A flat white metal plane is framed on a light's
// mirror reflection — a plane rather than a sphere, so a texel's angle on screen is its angle in
// reflection and the brightest texel sits on the peak. The lit HDR scene colour is read before
// tonemap, since the tonemapped output clamps to [0, 1] and can show neither the peak nor an
// overflow.

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

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

// glm's headers after Veng.h, which configures glm first.
#include <glm/gtc/constants.hpp>
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
    // The same pack's rippled material: the white metal with its shading normal tilted by a
    // sinusoid along both UV axes (rippled.frag), off until its RippleAmplitude is set.
    constexpr AssetId RippledInstanceId{0x80DA3AB8EEC00A11ULL};

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

    // How the camera frames the mirror: its vertical field of view and the target's extent.
    struct MirrorFraming
    {
        f32 FovY = glm::radians(4.0f);
        uvec2 Extent{128, 128};
    };

    // The distribution cases' framing: a texel spans about 0.03 degrees in reflection, against a
    // half-maximum lobe radius of about 0.12 degrees at the roughness floor.
    const MirrorFraming NarrowFraming{};

    // The light-size cases' framing: a texel spans about 0.08 degrees, so the smaller source's
    // half-maximum radius covers several texels, and the frame is wide enough that under 2 % of the
    // larger source's lobe energy falls outside it.
    const MirrorFraming WideFraming{.FovY = glm::radians(40.0f), .Extent = {512, 512}};

    // A metal plane at the origin, a light 45 degrees up behind it, and a camera on the light's
    // mirror reflection looking at the plane's centre. The ambient floor is zero, so a metal's lit
    // value is the light's reflection alone.
    struct MirrorScene
    {
        Unique<Scene> World;
        Unique<SceneRenderer> Renderer;
        CameraView Camera;
        MirrorFraming Framing;
        vec3 ToLight{0.0f};
        Entity Sun;
        // The view's specular anti-aliasing variance scale; unset renders at the view's default.
        std::optional<f32> SpecularAaVariance;
        // The flat ambient arm's floor; zero leaves a metal lit by its reflection of the light alone.
        vec3 AmbientFloor{0.0f};
    };

    MirrorScene MakeMirrorScene(Context& context, AssetManager& assets, TypeRegistry& types,
                                const AssetHandle<MaterialInstance>& material,
                                const MirrorFraming& framing = NarrowFraming)
    {
        MirrorScene mirror;
        mirror.World = Scene::Create(types);
        mirror.Framing = framing;

        const Ref<Mesh> plane = Mesh::BuildSync(
            context, Primitives::Plane(vec2(8.0f), uvec2(1), material), "Mirror Plane");
        const Entity ground = mirror.World->CreateEntity();
        mirror.World->Add<Transform>(ground);
        mirror.World->Add<MeshRenderer>(ground).Mesh = assets.Adopt(plane);

        mirror.ToLight = glm::normalize(vec3(0.0f, 1.0f, 1.0f));
        const vec3 toCamera = glm::normalize(vec3(0.0f, 1.0f, -1.0f));
        mirror.Sun = mirror.World->CreateEntity();
        // A point source, so the distribution cases see the surface's own lobe; the light-size
        // cases size it themselves.
        mirror.World->Add<Light>(mirror.Sun) = Light{
            .Type = LightType::Directional,
            .Direction = -mirror.ToLight,
            .Color = vec3(1.0f),
            .AngularRadius = 0.0f,
        };

        mirror.Camera.SetPerspective(framing.FovY, 1.0f, 0.1f, 100.0f);
        mirror.Camera.SetView(toCamera * 4.0f, vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

        mirror.Renderer = SceneRenderer::Create({
            .Context = context,
            .Assets = assets,
            .OutputFormat = context.GetOutputFormat(),
            .Extent = framing.Extent,
            .Settings = {.Mode = DebugView::Final, .Bloom = false, .Shadows = false, .AO = false},
        });
        return mirror;
    }

    // Renders the mirror scene as it stands and downloads the lit HDR target.
    vector<u8> RenderHdr(const Context& context, MirrorScene& mirror)
    {
        Renderer::SceneView view{.World = *mirror.World,
                                 .Camera = mirror.Camera,
                                 .Delta = 0.0f,
                                 .AmbientFloor = mirror.AmbientFloor};
        if (mirror.SpecularAaVariance.has_value())
        {
            view.SpecularAntiAliasingVariance = *mirror.SpecularAaVariance;
        }
        context.ImmediateCommands([&](CommandBuffer& cmd) { mirror.Renderer->Execute(cmd, view); });
        const Ref<ImageView> hdr = mirror.Renderer->GetHdrView();
        REQUIRE(hdr != nullptr);
        return hdr->GetImage()->Download();
    }

    // Sets the sun's radiance and the plane's roughness, renders, and scans the lit HDR target.
    HdrScan RenderMirror(const Context& context, MirrorScene& mirror, MaterialInstance& material,
                         f32 radiance, f32 roughness)
    {
        mirror.World->Get<Light>(mirror.Sun).Intensity = radiance / LuminousAnchor;
        material.SetParam("RoughnessFactor", roughness);
        return ScanHdr(RenderHdr(context, mirror));
    }

    // The lit HDR target as one grey value per texel (the mean of its colour channels), row-major.
    struct HdrImage
    {
        vector<f32> Values;
        uvec2 Extent{0};

        [[nodiscard]] f32 At(u32 x, u32 y) const { return Values[(y * Extent.x) + x]; }
    };

    HdrImage DecodeHdr(const vector<u8>& rgba16f, uvec2 extent)
    {
        HdrImage image{.Extent = extent};
        image.Values.resize(static_cast<usize>(extent.x) * extent.y);
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        for (usize i = 0; i < image.Values.size(); ++i)
        {
            image.Values[i] = (glm::unpackHalf1x16(halves[(i * 4) + 0]) +
                               glm::unpackHalf1x16(halves[(i * 4) + 1]) +
                               glm::unpackHalf1x16(halves[(i * 4) + 2])) /
                              3.0f;
        }
        return image;
    }

    // The angle off the camera's axis, along one screen axis, of a (fractional) texel coordinate.
    f32 TexelAngle(f32 coordinate, u32 extent, f32 fov)
    {
        const f32 ndc = (2.0f * (coordinate + 0.5f) / static_cast<f32>(extent)) - 1.0f;
        return std::atan(ndc * std::tan(0.5f * fov));
    }

    // The highlight's half-maximum radius in reflected angle: along the screen column through the
    // brightest texel, the half-maximum crossing on each side (interpolated between texels), and half
    // the angle between them. The column lies in the plane of incidence, where a reflected angle
    // maps one-to-one onto the half-vector's — across it the lobe is foreshortened by the cosine of
    // half the angle between the view and the light. Averaging the two sides cancels the slope
    // the view-dependent BRDF factors put across the lobe.
    f32 HalfMaxRadius(const HdrImage& image, f32 fov)
    {
        u32 peakX = 0;
        u32 peakY = 0;
        for (u32 y = 0; y < image.Extent.y; ++y)
        {
            for (u32 x = 0; x < image.Extent.x; ++x)
            {
                if (image.At(x, y) > image.At(peakX, peakY))
                {
                    peakX = x;
                    peakY = y;
                }
            }
        }
        const f32 half = 0.5f * image.At(peakX, peakY);

        const auto crossing = [&](i32 step) -> f32
        {
            i32 y = static_cast<i32>(peakY);
            while (y + step >= 0 && y + step < static_cast<i32>(image.Extent.y) &&
                   image.At(peakX, static_cast<u32>(y + step)) >= half)
            {
                y += step;
            }
            const i32 beyond = y + step;
            REQUIRE(beyond >= 0);
            REQUIRE(beyond < static_cast<i32>(image.Extent.y));
            const f32 inside = image.At(peakX, static_cast<u32>(y));
            const f32 outside = image.At(peakX, static_cast<u32>(beyond));
            const f32 t = (inside - half) / (inside - outside);
            return static_cast<f32>(y) + (t * static_cast<f32>(step));
        };

        const f32 low = TexelAngle(crossing(-1), image.Extent.y, fov);
        const f32 high = TexelAngle(crossing(+1), image.Extent.y, fov);
        return 0.5f * (high - low);
    }

    // The reflected energy reaching the frame: each texel's radiance weighted by the solid angle it
    // subtends, which falls off as cos³ of its angle off the axis (up to a constant per framing).
    f64 ReflectedEnergy(const HdrImage& image, f32 fov)
    {
        const f64 tanHalf = std::tan(0.5 * static_cast<f64>(fov));
        f64 energy = 0.0;
        for (u32 y = 0; y < image.Extent.y; ++y)
        {
            for (u32 x = 0; x < image.Extent.x; ++x)
            {
                const f64 tx = ((2.0 * (x + 0.5) / image.Extent.x) - 1.0) * tanHalf;
                const f64 ty = ((2.0 * (y + 0.5) / image.Extent.y) - 1.0) * tanHalf;
                const f64 solidAngle = 1.0 / std::pow(1.0 + (tx * tx) + (ty * ty), 1.5);
                energy += static_cast<f64>(image.At(x, y)) * solidAngle;
            }
        }
        return energy;
    }

    // Renders the mirror and measures its highlight: half-maximum radius and reflected energy.
    struct Highlight
    {
        f32 Radius = 0.0f;
        f64 Energy = 0.0;
        f32 Peak = 0.0f;
    };

    Highlight MeasureHighlight(const Context& context, MirrorScene& mirror)
    {
        const HdrImage image = DecodeHdr(RenderHdr(context, mirror), mirror.Framing.Extent);
        return Highlight{
            .Radius = HalfMaxRadius(image, mirror.Framing.FovY),
            .Energy = ReflectedEnergy(image, mirror.Framing.FovY),
            .Peak = *std::ranges::max_element(image.Values),
        };
    }

    AssetHandle<MaterialInstance> LoadWhiteMetal(AssetManager& assets, const path& archive,
                                                 const AssetId instance = WhitePlaneInstanceId)
    {
        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        const VoidResult cookResult = Veng::TestSupport::CookCached(
            cooker, path(GPU_GBUFFER_FIXTURE_DIR) / "white_plane_pack.json", archive, {}, nullptr,
            nullptr, nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR));
        REQUIRE(cookResult.has_value());
        REQUIRE(assets.Mount(archive).has_value());

        const AssetResult<AssetHandle<MaterialInstance>> material =
            assets.LoadSync<MaterialInstance>(instance);
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

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "specular lobe: a directional light's highlight is its angular size, and widening "
    "it conserves the reflected energy")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    const path archive = Veng::TestSupport::TempDir() / "veng_gpu_specular_sun_size.vengpack";
    const AssetHandle<MaterialInstance> material = LoadWhiteMetal(assets, archive);
    MirrorScene mirror = MakeMirrorScene(Context, assets, Types, material, WideFraming);
    auto& instance = const_cast<MaterialInstance&>(*material.Get());

    // At the roughness floor the surface's own lobe is far narrower than either source, so the
    // highlight a mirror shows is the source's: its half-maximum radius in reflected angle is the
    // source's angular radius. The light is at solar radiance, so neither peak meets the guard.
    instance.SetParam("RoughnessFactor", 0.04f);
    auto& sun = mirror.World->Get<Light>(mirror.Sun);
    sun.Intensity = 1.0f / LuminousAnchor;

    constexpr std::array<f32, 2> angularRadii{0.01f, 0.03f};
    std::array<Highlight, angularRadii.size()> highlights{};
    for (usize i = 0; i < angularRadii.size(); ++i)
    {
        sun.AngularRadius = angularRadii[i];
        highlights[i] = MeasureHighlight(Context, mirror);
    }

    for (usize i = 0; i < angularRadii.size(); ++i)
    {
        CAPTURE(i);
        CHECK(highlights[i].Peak < 0.5f * LightingOutputMax);
        CHECK(highlights[i].Radius >= 0.75f * angularRadii[i]);
        CHECK(highlights[i].Radius <= 1.25f * angularRadii[i]);
    }
    // A normalised distribution stays normalised as it widens: three times the size spreads the
    // same reflected energy over nine times the area.
    CHECK(highlights[1].Energy == doctest::Approx(highlights[0].Energy).epsilon(0.05));

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "specular lobe: a point light's highlight widens with its source radius")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    const path archive = Veng::TestSupport::TempDir() / "veng_gpu_specular_bulb_size.vengpack";
    const AssetHandle<MaterialInstance> material = LoadWhiteMetal(assets, archive);
    MirrorScene mirror = MakeMirrorScene(Context, assets, Types, material, WideFraming);
    auto& instance = const_cast<MaterialInstance&>(*material.Get());
    instance.SetParam("RoughnessFactor", 0.04f);

    // A bulb ten units out along the light's direction, its lumens set for about solar radiance at
    // the plane's centre. Its radius subtends 0.01 and then 0.03 rad there, and the highlight's
    // half-maximum — the surface's own lobe being far narrower than either — follows that size.
    constexpr f32 distance = 10.0f;
    mirror.World->Add<Transform>(mirror.Sun, Transform{.Position = mirror.ToLight * distance});
    auto& bulb = mirror.World->Get<Light>(mirror.Sun);
    bulb.Type = LightType::Point;
    bulb.Range = 100.0f;
    bulb.Intensity = 4.0f * glm::pi<f32>() * distance * distance / LuminousAnchor;

    constexpr std::array<f32, 2> radii{0.1f, 0.3f};
    std::array<Highlight, radii.size()> highlights{};
    for (usize i = 0; i < radii.size(); ++i)
    {
        bulb.Radius = radii[i];
        highlights[i] = MeasureHighlight(Context, mirror);
    }

    CHECK(highlights[0].Radius > 0.0f);
    CHECK(highlights[1].Radius > 2.0f * highlights[0].Radius);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "specular anti-aliasing: a flat mirror renders identically with it on and off")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    const path archive = Veng::TestSupport::TempDir() / "veng_gpu_specular_aa_flat.vengpack";
    const AssetHandle<MaterialInstance> material = LoadWhiteMetal(assets, archive);
    MirrorScene mirror = MakeMirrorScene(Context, assets, Types, material);
    auto& instance = const_cast<MaterialInstance&>(*material.Get());
    instance.SetParam("RoughnessFactor", 0.04f);
    mirror.World->Get<Light>(mirror.Sun).Intensity = 1.0f / LuminousAnchor;

    // A flat plane's normal does not vary across a pixel, so the widening adds exactly nothing and
    // the floor-roughness glint is untouched, however sharp.
    const vector<u8> on = RenderHdr(Context, mirror);
    mirror.SpecularAaVariance = 0.0f;
    const vector<u8> off = RenderHdr(Context, mirror);
    CHECK(ScanHdr(on).Max > 1.0f);
    CHECK(on == off);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "specular anti-aliasing: a rippled mirror's highlight holds its energy under a "
                  "half-pixel move")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    const path archive = Veng::TestSupport::TempDir() / "veng_gpu_specular_aa_ripple.vengpack";
    const AssetHandle<MaterialInstance> material =
        LoadWhiteMetal(assets, archive, RippledInstanceId);
    MirrorScene mirror = MakeMirrorScene(Context, assets, Types, material, WideFraming);
    auto& instance = const_cast<MaterialInstance&>(*material.Get());

    // A floor-roughness metal whose normal tilts by up to about three degrees with a period of three
    // to four texels, under a point-sized sun: the bare lobe is far narrower than the normal's
    // variation across one texel, so which texels catch it depends on where the ripple falls on the
    // grid. The period is kept above two texels, where the derivatives that measure the variation
    // still resolve it; finer detail is a normal map's to pre-filter.
    instance.SetParam("RoughnessFactor", 0.04f);
    instance.SetParam("RippleFrequency", 400.0f);
    instance.SetParam("RippleAmplitude", 0.05f);
    mirror.World->Get<Light>(mirror.Sun).Intensity = 1.0f / LuminousAnchor;

    const f64 before = MeasureHighlight(Context, mirror).Energy;

    // Move the camera half a texel sideways (its right axis is world X), shifting the ripple half a
    // texel across the grid while the reflection direction at every texel stays put.
    const f32 texel = 2.0f * 4.0f * std::tan(0.5f * mirror.Framing.FovY) /
                      static_cast<f32>(mirror.Framing.Extent.x);
    const vec3 shift(0.5f * texel, 0.0f, 0.0f);
    const vec3 eye = glm::normalize(vec3(0.0f, 1.0f, -1.0f)) * 4.0f;
    mirror.Camera.SetView(eye + shift, shift, vec3(0.0f, 1.0f, 0.0f));
    const f64 after = MeasureHighlight(Context, mirror).Energy;

    // The widened lobe covers the normal's variation across a texel, so the reflected energy the
    // frame catches is a property of the surface rather than of the sample grid. Measured within
    // 0.6 % across eight quarter-texel steps at landing, where the bare lobe swung by nearly a factor
    // of two; 2 % leaves headroom for driver rounding and still fails a lobe that aliases.
    REQUIRE(before > 0.0);
    CHECK(std::abs(after - before) <= 0.02 * before);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "specular anti-aliasing: a sphere's silhouette against the background is not "
                  "roughened")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    const path archive = Veng::TestSupport::TempDir() / "veng_gpu_specular_aa_silhouette.vengpack";
    const AssetHandle<MaterialInstance> material = LoadWhiteMetal(assets, archive);
    auto& instance = const_cast<MaterialInstance&>(*material.Get());
    instance.SetParam("RoughnessFactor", 0.3f);

    // A glossy metal sphere lit from the side, so its limb carries roughness-sensitive specular,
    // against an empty background. The flat ambient floor lifts every texel of the sphere above
    // zero, so a texel the lighting pass left black is background.
    constexpr uvec2 extent{128, 128};
    MirrorScene sphere;
    sphere.World = Scene::Create(Types);
    sphere.Framing = MirrorFraming{.FovY = glm::radians(40.0f), .Extent = extent};
    sphere.AmbientFloor = vec3(0.1f);
    const Ref<Mesh> mesh =
        Mesh::BuildSync(Context, Primitives::Sphere(1.0f, 48, 96, material), "Silhouette Sphere");
    const Entity ball = sphere.World->CreateEntity();
    sphere.World->Add<Transform>(ball);
    sphere.World->Add<MeshRenderer>(ball).Mesh = assets.Adopt(mesh);
    sphere.Sun = sphere.World->CreateEntity();
    sphere.World->Add<Light>(sphere.Sun) = Light{
        .Type = LightType::Directional,
        .Direction = -glm::normalize(vec3(1.0f, 0.4f, 0.3f)),
        .Color = vec3(1.0f),
        .Intensity = 1.0f / LuminousAnchor,
    };
    sphere.Camera.SetPerspective(sphere.Framing.FovY, 1.0f, 0.1f, 100.0f);
    sphere.Camera.SetView(vec3(0.0f, 0.0f, 4.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));
    sphere.Renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = extent,
        .Settings = {.Mode = DebugView::Final, .Bloom = false, .Shadows = false, .AO = false},
    });

    const HdrImage on = DecodeHdr(RenderHdr(Context, sphere), extent);
    sphere.SpecularAaVariance = 0.0f;
    const HdrImage off = DecodeHdr(RenderHdr(Context, sphere), extent);
    const auto background = [&](u32 x, u32 y) { return on.At(x, y) <= 0.0f; };

    // Every lit texel whose 2x2 derivative quad holds a background texel shades as with the widening
    // off; the sphere's own curvature is real variance, so some interior texel differs.
    u32 silhouette = 0;
    u32 silhouetteChanged = 0;
    u32 interiorChanged = 0;
    for (u32 y = 0; y < extent.y; ++y)
    {
        for (u32 x = 0; x < extent.x; ++x)
        {
            if (background(x, y))
            {
                continue;
            }
            const u32 qx = x & ~1u;
            const u32 qy = y & ~1u;
            const bool edge = background(qx, qy) || background(qx + 1, qy) ||
                              background(qx, qy + 1) || background(qx + 1, qy + 1);
            const bool changed = on.At(x, y) != off.At(x, y);
            silhouette += edge ? 1u : 0u;
            silhouetteChanged += (edge && changed) ? 1u : 0u;
            interiorChanged += (!edge && changed) ? 1u : 0u;
        }
    }
    CHECK(silhouette > 20u);
    CHECK(silhouetteChanged == 0u);
    CHECK(interiorChanged > 0u);

    std::filesystem::remove(archive);
}
