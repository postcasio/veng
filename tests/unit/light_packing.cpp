// Light-packing unit cases. PackSceneLights is the device-free CPU core of
// SceneRenderer's per-frame lighting setup (Execute calls it, then uploads the
// result): cascade-set and punctual shadow-slot assignment by estimated contribution,
// cone-cosine packing, and the std430 light layout. Pure scene-query + glm math — no
// Context, no driver — so the branchy slot/cap/record logic a golden image only
// exercises incidentally is testable here.

#include <doctest/doctest.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cmath>
#include <limits>

#include <Veng/Math/Frustum.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/LightPacking.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    void RegisterBuiltins(TypeRegistry& types)
    {
        types.Register<Name>("Name");
        types.Register<Transform>("Transform");
        types.Register<Hierarchy>("Hierarchy");
        types.Register<Light>("Light");
    }

    // The flags word's cascade-set index, decoded exactly as the lighting shader decodes it.
    u32 CascadeSetOf(const PackedLight& light)
    {
        return (static_cast<u32>(light.Cone.w) & LightFlags::CascadeSetMask) >>
               LightFlags::CascadeSetShift;
    }

    bool IsCascadeDenied(const PackedLight& light)
    {
        return (static_cast<u32>(light.Cone.w) & LightFlags::CascadeDenied) != 0;
    }

    bool IsAreaCascadeShadowed(const PackedLight& light)
    {
        return (static_cast<u32>(light.Cone.w) & LightFlags::AreaCascadeShadowed) != 0;
    }

    // Adds a light at a world position via its Transform (the packer reads the light's
    // position from the entity's world matrix, never the component).
    Entity AddLight(Scene& scene, const Light& light, const vec3& position = vec3(0.0f))
    {
        const Entity e = scene.CreateEntity();
        scene.Add<Transform>(e, Transform{.Position = position});
        scene.Add<Light>(e, light);
        return e;
    }
}

TEST_CASE("PackSceneLights: an empty scene packs nothing and reports no directional")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const Unique<Scene> scene = Scene::Create(types);

    const PackedSceneLights packed = PackSceneLights(*scene, true, 1024);

    CHECK(packed.LightCount == 0);
    CHECK(packed.PunctualCount == 0);
    CHECK(packed.CascadeSetCount == 0);
    CHECK(packed.DeniedDirectionalCount == 0);
    // The default travel is straight down, so a scene with no light still drives a
    // sensible cascade matrix.
    CHECK(packed.CascadeTravel[0] == vec3{0.0f, -1.0f, 0.0f});
}

TEST_CASE("PackSceneLights: a lone directional packs into cascade set 0 with no atlas slot")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const Unique<Scene> scene = Scene::Create(types);

    AddLight(*scene, Light{.Type = LightType::Directional,
                           .Direction = vec3(0.3f, -0.8f, 0.5f),
                           .Color = vec3(1.0f, 0.9f, 0.8f),
                           .Intensity = 2.0f});

    const PackedSceneLights packed = PackSceneLights(*scene, true, 1024);

    REQUIRE(packed.LightCount == 1);
    CHECK(packed.CascadeSetCount == 1);
    CHECK(packed.CascadeTravel[0] == vec3{0.3f, -0.8f, 0.5f});

    // The overwhelmingly common scene — one shadow-casting directional: set 0 is the flags word's
    // zero, and the punctual slot stays -1 because a directional never takes an atlas tile. Its
    // packed radiance is the authored lux converted through the anchor — a directional carries
    // `Intensity · S`.
    const PackedLight& light = packed.Lights[0];
    CHECK(light.DirectionType.w == doctest::Approx(0.0f)); // LightType::Directional
    CHECK(light.ColorIntensity.a == doctest::Approx(2.0f * LuminousAnchor));
    CHECK(light.Cone.z == doctest::Approx(-1.0f));
    CHECK(light.Cone.w == doctest::Approx(0.0f));
    CHECK(light.Area.w == doctest::Approx(-1.0f));
    CHECK(light.AreaNormal.w == doctest::Approx(0.0f));
    CHECK(packed.PunctualCount == 0);
    CHECK(packed.DeniedDirectionalCount == 0);
}

TEST_CASE("PackSceneLights: a directional's angular radius packs into Area.x as its sine")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const Unique<Scene> scene = Scene::Create(types);

    // Told apart by intensity, since the packer is free to order lights as it likes. A default
    // directional carries the Sun's size; zero stays a point, and a radius past a quarter turn
    // saturates at a hemisphere rather than folding back.
    AddLight(*scene, Light{.Type = LightType::Directional, .Intensity = 1.0f});
    AddLight(*scene,
             Light{.Type = LightType::Directional, .Intensity = 2.0f, .AngularRadius = 0.0f});
    AddLight(*scene,
             Light{.Type = LightType::Directional, .Intensity = 3.0f, .AngularRadius = 0.03f});
    AddLight(*scene,
             Light{.Type = LightType::Directional, .Intensity = 4.0f, .AngularRadius = 2.5f});

    const PackedSceneLights packed = PackSceneLights(*scene, true, 1024);

    REQUIRE(packed.LightCount == 4);
    std::array<f32, 4> sines{-1.0f, -1.0f, -1.0f, -1.0f};
    for (u32 i = 0; i < packed.LightCount; ++i)
    {
        const auto slot =
            static_cast<usize>(std::lround(packed.Lights[i].ColorIntensity.a / LuminousAnchor) - 1);
        REQUIRE(slot < sines.size());
        sines[slot] = packed.Lights[i].Area.x;
    }
    CHECK(sines[0] == doctest::Approx(std::sin(0.004675f)));
    CHECK(sines[1] == 0.0f);
    CHECK(sines[2] == doctest::Approx(std::sin(0.03f)));
    CHECK(sines[3] == doctest::Approx(1.0f));
}

TEST_CASE("PackSceneLights: a spot aims with its entity; a directional keeps its world direction")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const Unique<Scene> scene = Scene::Create(types);

    // A 90° yaw about +Y sends local -Z (the authored aim below) to world -X.
    const quat yaw = glm::angleAxis(glm::half_pi<f32>(), vec3(0.0f, 1.0f, 0.0f));

    // A spot authored to aim along local -Z, on a yawed entity: the packed aim is the
    // entity-rotated world direction, not the authored local vector.
    const Entity spot = scene->CreateEntity();
    scene->Add<Transform>(spot, Transform{.Rotation = yaw});
    scene->Add<Light>(spot, Light{.Type = LightType::Spot,
                                  .Direction = vec3(0.0f, 0.0f, -1.0f),
                                  .Range = 8.0f,
                                  .OuterCone = 0.5f});

    // A directional with the same entity rotation keeps its authored world-space direction —
    // an infinite source is a world phenomenon, exempt from the entity-relative rotation.
    const Entity sun = scene->CreateEntity();
    scene->Add<Transform>(sun, Transform{.Rotation = yaw});
    scene->Add<Light>(sun, Light{.Type = LightType::Directional,
                                 .Direction = vec3(0.0f, 0.0f, -1.0f),
                                 .Intensity = 100000.0f});

    // A Rect panel aims along its emitting face (local +Z) through the entity transform, so its
    // packed direction is the yawed normal (world +X) regardless of any authored Direction.
    const Entity panel = scene->CreateEntity();
    scene->Add<Transform>(panel, Transform{.Rotation = yaw});
    scene->Add<Light>(panel, Light{.Type = LightType::Rect,
                                   .Direction = vec3(0.0f, 0.0f, -1.0f),
                                   .Range = 8.0f,
                                   .Width = 1.0f,
                                   .Height = 1.0f});

    const PackedSceneLights packed = PackSceneLights(*scene, true, 1024);
    REQUIRE(packed.LightCount == 3);

    const vec3 spotDir = vec3(packed.Lights[0].DirectionType);
    CHECK(spotDir.x == doctest::Approx(-1.0f).epsilon(0.001));
    CHECK(std::abs(spotDir.y) < 1e-4f);
    CHECK(std::abs(spotDir.z) < 1e-4f);

    const vec3 sunDir = vec3(packed.Lights[1].DirectionType);
    CHECK(std::abs(sunDir.x) < 1e-4f);
    CHECK(std::abs(sunDir.y) < 1e-4f);
    CHECK(sunDir.z == doctest::Approx(-1.0f).epsilon(0.001));

    const vec3 panelDir = vec3(packed.Lights[2].DirectionType);
    CHECK(panelDir.x == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(std::abs(panelDir.y) < 1e-4f);
    CHECK(std::abs(panelDir.z) < 1e-4f);
}

TEST_CASE(
    "PackSceneLights: each light type converts its photometric intensity to internal radiance")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const f32 pi = glm::pi<f32>();

    // A directional of L lux packs L · S.
    {
        const Unique<Scene> scene = Scene::Create(types);
        const f32 lux = 50000.0f;
        AddLight(*scene, Light{.Type = LightType::Directional, .Intensity = lux});
        const PackedSceneLights packed = PackSceneLights(*scene, false, 1024);
        REQUIRE(packed.LightCount == 1);
        CHECK(packed.Lights[0].ColorIntensity.a == doctest::Approx(lux * LuminousAnchor));
    }

    // A point of Φ lumens packs Φ/(4π) · S — the luminous power spread over the whole sphere. That
    // packed value is the internal radiance a surface at one metre receives, since the shader then
    // applies the 1/d² falloff (1 at d = 1).
    {
        const Unique<Scene> scene = Scene::Create(types);
        const f32 lumens = 1200.0f;
        AddLight(*scene, Light{.Type = LightType::Point, .Intensity = lumens, .Range = 10.0f});
        const PackedSceneLights packed = PackSceneLights(*scene, false, 1024);
        REQUIRE(packed.LightCount == 1);
        CHECK(packed.Lights[0].ColorIntensity.a ==
              doctest::Approx(lumens / (4.0f * pi) * LuminousAnchor));
    }

    // A spot of Φ lumens at outer half-angle θ packs Φ/(2π(1−cos θ)) · S, and halving the cone's
    // solid angle at fixed lumens doubles the packed radiance — the same power in a tighter cone.
    {
        const Unique<Scene> scene = Scene::Create(types);
        const f32 lumens = 900.0f;
        const f32 wide = 0.9f;
        AddLight(*scene, Light{.Type = LightType::Spot, .Intensity = lumens, .OuterCone = wide});
        const PackedSceneLights packed = PackSceneLights(*scene, false, 1024);
        REQUIRE(packed.LightCount == 1);
        const f32 wideSolid = 2.0f * pi * (1.0f - std::cos(wide));
        CHECK(packed.Lights[0].ColorIntensity.a ==
              doctest::Approx(lumens / wideSolid * LuminousAnchor));

        // The half-solid-angle cone: cos θ_narrow = (1 + cos θ_wide)/2 halves 2π(1−cos θ).
        const f32 narrow = std::acos(0.5f * (1.0f + std::cos(wide)));
        const Unique<Scene> narrowScene = Scene::Create(types);
        AddLight(*narrowScene,
                 Light{.Type = LightType::Spot, .Intensity = lumens, .OuterCone = narrow});
        const PackedSceneLights narrowPacked = PackSceneLights(*narrowScene, false, 1024);
        REQUIRE(narrowPacked.LightCount == 1);
        CHECK(narrowPacked.Lights[0].ColorIntensity.a ==
              doctest::Approx(2.0f * packed.Lights[0].ColorIntensity.a));
    }

    // An area light of N nits packs N · S for Rect, Sphere, and Polygon alike — the area path treats
    // Colour·Intensity as the emitter luminance directly, so there is no solid-angle division.
    {
        const f32 nits = 3000.0f;
        for (const LightType type : {LightType::Rect, LightType::Sphere, LightType::Polygon})
        {
            const Unique<Scene> scene = Scene::Create(types);
            Light light{.Type = type, .Intensity = nits, .Radius = 1.0f};
            if (type == LightType::Polygon)
            {
                light.PolygonVertices = {vec3(-1.0f, -1.0f, 0.0f), vec3(1.0f, -1.0f, 0.0f),
                                         vec3(0.0f, 1.0f, 0.0f)};
            }
            AddLight(*scene, light);
            const PackedSceneLights packed = PackSceneLights(*scene, false, 1024);
            REQUIRE(packed.LightCount == 1);
            CHECK(packed.Lights[0].ColorIntensity.a == doctest::Approx(nits * LuminousAnchor));
        }
    }
}

TEST_CASE("PackSceneLights: the two units agree at the anchor, through one shared scale")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const f32 pi = glm::pi<f32>();

    // A point authored at the lumens that produce a given illuminance E at one metre, and a
    // directional authored at that same E in lux, deliver equal internal radiance to a surface at
    // one metre: the point packs Φ/(4π)·S and Φ = 4π·E, so its packed radiance is E·S, exactly the
    // directional's. The two units are consistent by construction of the anchor.
    const f32 illuminance = 800.0f; // lux, i.e. lumens/m² at one metre
    const Unique<Scene> pointScene = Scene::Create(types);
    AddLight(*pointScene,
             Light{.Type = LightType::Point, .Intensity = 4.0f * pi * illuminance, .Range = 10.0f});
    const Unique<Scene> dirScene = Scene::Create(types);
    AddLight(*dirScene, Light{.Type = LightType::Directional, .Intensity = illuminance});

    const PackedSceneLights point = PackSceneLights(*pointScene, false, 1024);
    const PackedSceneLights dir = PackSceneLights(*dirScene, false, 1024);
    REQUIRE(point.LightCount == 1);
    REQUIRE(dir.LightCount == 1);
    CHECK(point.Lights[0].ColorIntensity.a == doctest::Approx(dir.Lights[0].ColorIntensity.a));
    CHECK(dir.Lights[0].ColorIntensity.a == doctest::Approx(illuminance * LuminousAnchor));

    // The anchor is the single knob: for every type the packed radiance is its type-geometry times
    // the one shared scale S, so the ratio packed / (geometry) is LuminousAnchor for all of them —
    // scaling S would scale every type's internal radiance by the same factor.
    const f32 dirRatio = dir.Lights[0].ColorIntensity.a / illuminance;
    const f32 pointRatio =
        point.Lights[0].ColorIntensity.a / ((4.0f * pi * illuminance) / (4.0f * pi));
    CHECK(dirRatio == doctest::Approx(LuminousAnchor));
    CHECK(pointRatio == doctest::Approx(LuminousAnchor));
}

TEST_CASE("PackSceneLights: a point/spot source radius packs into Area.x, transform-scaled")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const Unique<Scene> scene = Scene::Create(types);

    // A Point light with an authored source radius, on an entity uniformly scaled 2x: the packer
    // scales the radius by the transform basis so a parented, scaled light keeps a consistent size.
    const Entity point = scene->CreateEntity();
    scene->Add<Transform>(point, Transform{.Scale = vec3(2.0f)});
    scene->Add<Light>(point, Light{.Type = LightType::Point, .Range = 5.0f, .Radius = 0.5f});

    // A Spot with an unscaled transform: the radius packs through unchanged.
    AddLight(*scene,
             Light{.Type = LightType::Spot, .Range = 5.0f, .OuterCone = 0.6f, .Radius = 0.3f},
             vec3(10.0f, 0.0f, 0.0f));

    const PackedSceneLights packed = PackSceneLights(*scene, true, 1024);

    REQUIRE(packed.LightCount == 2);
    CHECK(packed.Lights[0].Area.x == doctest::Approx(1.0f)); // 0.5 * 2x scale
    CHECK(packed.Lights[1].Area.x == doctest::Approx(0.3f));
}

TEST_CASE("PackSceneLights: a light's specular scale packs into Response.x, 1 unless authored")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const Unique<Scene> scene = Scene::Create(types);

    AddLight(*scene, Light{.Type = LightType::Point, .Range = 5.0f}, vec3(0.0f));
    AddLight(*scene,
             Light{.Type = LightType::Rect,
                   .Range = 5.0f,
                   .Width = 1.0f,
                   .Height = 1.0f,
                   .SpecularScale = 0.0f},
             vec3(3.0f, 0.0f, 0.0f));

    const PackedSceneLights packed = PackSceneLights(*scene, true, 1024);

    REQUIRE(packed.LightCount == 2);
    for (u32 i = 0; i < packed.LightCount; ++i)
    {
        const bool isRect = packed.Lights[i].DirectionType.w == static_cast<f32>(LightType::Rect);
        CHECK(packed.Lights[i].Response.x == (isRect ? 0.0f : 1.0f));
    }
}

TEST_CASE("PackSceneLights: a default point/spot source radius is zero, so the near field is inert")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const Unique<Scene> scene = Scene::Create(types);

    // With Radius at its default, the packed source radius is zero: the shader's distance clamp
    // falls back to the epsilon floor, reproducing the pure inverse-square.
    AddLight(*scene, Light{.Type = LightType::Point, .Range = 5.0f});
    AddLight(*scene, Light{.Type = LightType::Spot, .Range = 5.0f, .OuterCone = 0.6f},
             vec3(10.0f, 0.0f, 0.0f));

    const PackedSceneLights packed = PackSceneLights(*scene, true, 1024);

    REQUIRE(packed.LightCount == 2);
    CHECK(packed.Lights[0].Area.x == doctest::Approx(0.0f));
    CHECK(packed.Lights[1].Area.x == doctest::Approx(0.0f));
}

TEST_CASE("PackSceneLights: two directionals each take a cascade set of their own")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const Unique<Scene> scene = Scene::Create(types);

    AddLight(*scene, Light{.Type = LightType::Directional,
                           .Direction = vec3(1.0f, 0.0f, 0.0f),
                           .Intensity = 3.0f});
    AddLight(*scene, Light{.Type = LightType::Directional,
                           .Direction = vec3(0.0f, 0.0f, 1.0f),
                           .Intensity = 1.0f});

    const PackedSceneLights packed = PackSceneLights(*scene, true, 1024);

    REQUIRE(packed.LightCount == 2);
    REQUIRE(packed.CascadeSetCount == 2);
    CHECK(packed.DeniedDirectionalCount == 0);
    // Brightest first: each set is fit to its own light's direction, so a scene with two
    // comparable suns shadows from both rather than from whichever arrived first.
    CHECK(packed.CascadeTravel[0] == vec3{1.0f, 0.0f, 0.0f});
    CHECK(packed.CascadeTravel[1] == vec3{0.0f, 0.0f, 1.0f});
    // And the two lights name different sets, so neither samples the other's cascade.
    CHECK(CascadeSetOf(packed.Lights[0]) == 0);
    CHECK(CascadeSetOf(packed.Lights[1]) == 1);
    CHECK_FALSE(IsCascadeDenied(packed.Lights[0]));
    CHECK_FALSE(IsCascadeDenied(packed.Lights[1]));
}

TEST_CASE("PackSceneLights: entity order does not decide which light drives a cascade set")
{
    TypeRegistry types;
    RegisterBuiltins(types);

    // The same three directionals of clearly different brightness, submitted in three orders.
    // Every order must produce the same set-0 and set-1 sources, and deny the same light.
    const std::array<Light, 3> lights{
        Light{.Type = LightType::Directional,
              .Direction = vec3(1.0f, 0.0f, 0.0f),
              .Intensity = 0.25f},
        Light{
            .Type = LightType::Directional, .Direction = vec3(0.0f, 1.0f, 0.0f), .Intensity = 8.0f},
        Light{
            .Type = LightType::Directional, .Direction = vec3(0.0f, 0.0f, 1.0f), .Intensity = 2.0f},
    };
    for (const std::array<u32, 3>& order :
         {std::array<u32, 3>{0, 1, 2}, std::array<u32, 3>{2, 1, 0}, std::array<u32, 3>{1, 2, 0}})
    {
        const Unique<Scene> scene = Scene::Create(types);
        for (const u32 i : order)
        {
            AddLight(*scene, lights[i]);
        }

        const PackedSceneLights packed = PackSceneLights(*scene, true, 1024);
        REQUIRE(packed.CascadeSetCount == 2);
        CHECK(packed.CascadeTravel[0] == vec3{0.0f, 1.0f, 0.0f}); // the 8.0 light
        CHECK(packed.CascadeTravel[1] == vec3{0.0f, 0.0f, 1.0f}); // the 2.0 light
        CHECK(packed.DeniedDirectionalCount == 1);
    }
}

TEST_CASE("PackSceneLights: a directional past the cascade budget is denied, explicitly")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const Unique<Scene> scene = Scene::Create(types);

    // One more shadow-casting directional than there are cascade sets, dimmest last.
    for (u32 i = 0; i < MaxCascadeSets + 1; ++i)
    {
        AddLight(*scene, Light{.Type = LightType::Directional,
                               .Direction = vec3(0.0f, -1.0f, 0.0f),
                               .Intensity = 4.0f - static_cast<f32>(i)});
    }

    const PackedSceneLights packed = PackSceneLights(*scene, true, 1024);

    REQUIRE(packed.LightCount == MaxCascadeSets + 1);
    CHECK(packed.CascadeSetCount == MaxCascadeSets);
    CHECK(packed.DeniedDirectionalCount == 1);
    // The dimmest carries the denial in its own flags word — it shades unshadowed and says so,
    // rather than reading set 0's cascade, which is fit to a different light's direction.
    const PackedLight& denied = packed.Lights[MaxCascadeSets];
    CHECK(IsCascadeDenied(denied));
    CHECK(CascadeSetOf(denied) == 0);
    CHECK(denied.Cone.z == doctest::Approx(-1.0f));
    // A denied directional takes no punctual tile either: a perspective map is not a shadow a
    // parallel source has.
    CHECK(packed.PunctualCount == 0);
}

TEST_CASE("PackSceneLights: the punctual budget goes to the brightest lights, near or far")
{
    TypeRegistry types;
    RegisterBuiltins(types);

    // A dim light created first and a bright one created second, both well inside the bound.
    const Unique<Scene> equidistant = Scene::Create(types);
    AddLight(*equidistant, Light{.Type = LightType::Point, .Intensity = 0.1f, .Range = 100.0f},
             vec3(-4.0f, 0.0f, 0.0f));
    AddLight(*equidistant, Light{.Type = LightType::Point, .Intensity = 10.0f, .Range = 100.0f},
             vec3(4.0f, 0.0f, 0.0f));
    const AABB bounds{.Min = vec3(-5.0f), .Max = vec3(5.0f)};

    // With one slot's worth of budget the bright one would win outright; with the full budget
    // both are slotted, so the property under test is the *order* they were slotted in — slot 0
    // is the brighter, whatever order the scene created them in.
    const PackedSceneLights packed = PackSceneLights(*equidistant, true, 1024, bounds);
    REQUIRE(packed.PunctualCount == 2);
    CHECK(packed.Lights[1].Cone.z == doctest::Approx(0.0f)); // the bright one took slot 0
    CHECK(packed.Lights[0].Cone.z == doctest::Approx(1.0f));

    // And the order tracks distance, not just intensity: pull the bright light far outside the
    // bound and its inverse-square falloff drops it below the dim one standing in the middle.
    const Unique<Scene> separated = Scene::Create(types);
    AddLight(*separated, Light{.Type = LightType::Point, .Intensity = 0.1f, .Range = 1000.0f},
             vec3(0.0f, 0.0f, 0.0f));
    AddLight(*separated, Light{.Type = LightType::Point, .Intensity = 10.0f, .Range = 1000.0f},
             vec3(500.0f, 0.0f, 0.0f));
    const PackedSceneLights far = PackSceneLights(*separated, true, 1024, bounds);
    REQUIRE(far.PunctualCount == 2);
    CHECK(far.Lights[0].Cone.z == doctest::Approx(0.0f)); // the near dim one took slot 0
    CHECK(far.Lights[1].Cone.z == doctest::Approx(1.0f));
}

TEST_CASE("PackSceneLights: identically-contributing lights resolve the same way every pack")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const Unique<Scene> scene = Scene::Create(types);

    // Two directionals with identical contribution, and one more punctual caster than there are
    // slots, every one identical: nothing distinguishes them but scene iteration order, which is
    // the documented tie-break. Repeated packs of an unchanged scene must agree exactly — the
    // property that keeps a light from flickering between shadowed and unshadowed.
    AddLight(*scene, Light{.Type = LightType::Directional, .Direction = vec3(1.0f, 0.0f, 0.0f)});
    AddLight(*scene, Light{.Type = LightType::Directional, .Direction = vec3(0.0f, 0.0f, 1.0f)});
    for (u32 i = 0; i < MaxShadowedPunctual + 1; ++i)
    {
        AddLight(*scene, Light{.Type = LightType::Point}, vec3(0.0f, 0.0f, 0.0f));
    }

    const PackedSceneLights first = PackSceneLights(*scene, true, 1024);
    REQUIRE(first.CascadeSetCount == 2);
    REQUIRE(first.PunctualCount == MaxShadowedPunctual);
    // The tie-break is iteration order, so it is the *first* of each identical group that wins.
    CHECK(first.CascadeTravel[0] == vec3{1.0f, 0.0f, 0.0f});
    CHECK(first.Lights[first.LightCount - 1].Cone.z == doctest::Approx(-1.0f));

    for (u32 repeat = 0; repeat < 3; ++repeat)
    {
        const PackedSceneLights again = PackSceneLights(*scene, true, 1024);
        REQUIRE(again.LightCount == first.LightCount);
        CHECK(again.CascadeTravel[0] == first.CascadeTravel[0]);
        CHECK(again.CascadeTravel[1] == first.CascadeTravel[1]);
        u32 differing = 0;
        for (u32 i = 0; i < first.LightCount; ++i)
        {
            differing += again.Lights[i].Cone == first.Lights[i].Cone ? 0u : 1u;
        }
        CHECK(differing == 0);
    }
}

TEST_CASE(
    "PackSceneLights: cone half-angles are stored as cosines and position comes from the transform")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const Unique<Scene> scene = Scene::Create(types);

    const f32 inner = 0.3f;
    const f32 outer = 0.7f;
    AddLight(*scene,
             Light{.Type = LightType::Spot, .Range = 12.0f, .InnerCone = inner, .OuterCone = outer},
             vec3(4.0f, 5.0f, 6.0f));

    const PackedSceneLights packed = PackSceneLights(*scene, false, 1024);

    REQUIRE(packed.LightCount == 1);
    CHECK(packed.Lights[0].Cone.x == doctest::Approx(std::cos(inner)));
    CHECK(packed.Lights[0].Cone.y == doctest::Approx(std::cos(outer)));
    // PositionRange.xyz is the world position, .w the range.
    CHECK(packed.Lights[0].PositionRange.x == doctest::Approx(4.0f));
    CHECK(packed.Lights[0].PositionRange.y == doctest::Approx(5.0f));
    CHECK(packed.Lights[0].PositionRange.z == doctest::Approx(6.0f));
    CHECK(packed.Lights[0].PositionRange.w == doctest::Approx(12.0f));
}

TEST_CASE("PackSceneLights: punctual shadows off assigns no slots")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const Unique<Scene> scene = Scene::Create(types);

    AddLight(*scene, Light{.Type = LightType::Point});
    AddLight(*scene, Light{.Type = LightType::Spot});

    const PackedSceneLights packed = PackSceneLights(*scene, false, 1024);

    REQUIRE(packed.LightCount == 2);
    CHECK(packed.PunctualCount == 0);
    CHECK(packed.Lights[0].Cone.z == doctest::Approx(-1.0f));
    CHECK(packed.Lights[1].Cone.z == doctest::Approx(-1.0f));
}

TEST_CASE("PackSceneLights: a spot uses face 0 (type 2); a point uses all six faces (type 1)")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const Unique<Scene> scene = Scene::Create(types);

    AddLight(*scene, Light{.Type = LightType::Spot, .Range = 8.0f, .OuterCone = 0.6f});
    AddLight(*scene, Light{.Type = LightType::Point, .Range = 8.0f});

    const PackedSceneLights packed = PackSceneLights(*scene, true, 1024);

    // The value an unwritten face slot keeps (default-constructed by the result struct).
    const mat4 unset = PackedSceneLights{}.PunctualRawViewProj[0][0];

    REQUIRE(packed.PunctualCount == 2);
    // Slot 0: spot. Params.x == 2.0 marks a spot; only face 0's raw matrix is filled.
    CHECK(packed.PunctualRecords[0].Params.x == doctest::Approx(2.0f));
    CHECK(packed.PunctualRawViewProj[0][0] != unset);
    CHECK(packed.PunctualRawViewProj[0][1] == unset);
    // Slot 1: point. Params.x == 1.0 marks a point; all six faces' raw matrices are filled.
    CHECK(packed.PunctualRecords[1].Params.x == doctest::Approx(1.0f));
    for (u32 f = 0; f < CubeFaceCount; ++f)
    {
        CHECK(packed.PunctualRawViewProj[1][f] != unset);
    }
}

TEST_CASE("PackSceneLights: shadow slots are capped at MaxShadowedPunctual, the rest carry -1")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const Unique<Scene> scene = Scene::Create(types);

    // One more shadow-casting light than there are slots.
    const u32 count = MaxShadowedPunctual + 1;
    for (u32 i = 0; i < count; ++i)
    {
        AddLight(*scene, Light{.Type = LightType::Point, .Range = 5.0f},
                 vec3(static_cast<f32>(i), 0.0f, 0.0f));
    }

    const PackedSceneLights packed = PackSceneLights(*scene, true, 1024);

    REQUIRE(packed.LightCount == count);
    CHECK(packed.PunctualCount == MaxShadowedPunctual);

    // Exactly one packed light is left unshadowed (Cone.z == -1).
    u32 slotted = 0;
    u32 unslotted = 0;
    for (u32 i = 0; i < packed.LightCount; ++i)
    {
        if (packed.Lights[i].Cone.z < 0.0f)
        {
            ++unslotted;
        }
        else
        {
            ++slotted;
        }
    }
    CHECK(slotted == MaxShadowedPunctual);
    CHECK(unslotted == 1);
}

TEST_CASE("PackSceneLights: the packed light count is capped at MaxLights")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const Unique<Scene> scene = Scene::Create(types);

    const u32 count = Renderer::SceneView::MaxLights + 3;
    for (u32 i = 0; i < count; ++i)
    {
        AddLight(*scene, Light{.Type = LightType::Directional});
    }

    const PackedSceneLights packed = PackSceneLights(*scene, false, 1024);

    CHECK(packed.LightCount == Renderer::SceneView::MaxLights);
}

TEST_CASE("PackSceneLights: a caster bound tightens a spot light's shadow far plane")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const Unique<Scene> scene = Scene::Create(types);

    // A far-reaching spot light at the origin aimed down -Z. A spot always uses its perspective
    // tile (never the cascade path), so this exercises the scene-bound frustum fit.
    AddLight(*scene, Light{.Type = LightType::Spot,
                           .Direction = vec3(0.0f, 0.0f, -1.0f),
                           .Range = 10000.0f,
                           .OuterCone = 0.5f});

    // Without a bound the record's far is the full range.
    const PackedSceneLights unfitted = PackSceneLights(*scene, true, 1024);
    REQUIRE(unfitted.PunctualCount == 1);
    CHECK(unfitted.PunctualRecords[0].Params.z == doctest::Approx(10000.0f));

    // With a caster bound holding only a small far patch, the far pulls in to it.
    const AABB casterBounds{.Min = vec3(-2.0f, -2.0f, -102.0f), .Max = vec3(2.0f, 2.0f, -98.0f)};
    const PackedSceneLights fitted = PackSceneLights(*scene, true, 1024, casterBounds);
    REQUIRE(fitted.PunctualCount == 1);
    CHECK(fitted.PunctualRecords[0].Params.z < 200.0f);
    CHECK(fitted.PunctualRecords[0].Params.z == doctest::Approx(102.0f * 1.02f).epsilon(0.05));
    // The tightened frustum needs far less bias than the full-range one (its clamp ceiling).
    CHECK(fitted.PunctualRecords[0].Params.w < unfitted.PunctualRecords[0].Params.w);
}

TEST_CASE("PackSceneLights: a far Sphere area light is cascade-routed, not punctual-slotted")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const Unique<Scene> scene = Scene::Create(types);

    // A sphere light at the origin over a small, distant caster patch: the scene subtends a tiny
    // angle from the light (near-parallel), so it drives the cascade atlas instead of a tile.
    AddLight(*scene, Light{.Type = LightType::Sphere,
                           .Direction = vec3(0.0f, 0.0f, -1.0f),
                           .Range = 10000.0f,
                           .Radius = 50.0f});
    const AABB farBounds{.Min = vec3(-2.0f, -2.0f, -102.0f), .Max = vec3(2.0f, 2.0f, -98.0f)};

    const PackedSceneLights packed = PackSceneLights(*scene, true, 1024, farBounds);
    REQUIRE(packed.LightCount == 1);
    // It takes no punctual slot and instead drives a cascade set of its own.
    CHECK(packed.PunctualCount == 0);
    CHECK(packed.CascadeSetCount == 1);
    CHECK(packed.CascadeTravel[0] == vec3(0.0f, 0.0f, -1.0f));
    // The flags word marks the light cascade-shadowed for the lighting pass, since an area
    // light's shadow arm cannot be read off its type.
    CHECK(IsAreaCascadeShadowed(packed.Lights[0]));

    // The same light over a bound it sits close to (the scene subtends a wide angle) stays on its
    // perspective tile — the direction genuinely diverges, so cascades would be wrong.
    const Unique<Scene> near = Scene::Create(types);
    AddLight(*near, Light{.Type = LightType::Sphere,
                          .Direction = vec3(0.0f, 0.0f, -1.0f),
                          .Range = 10000.0f,
                          .Radius = 50.0f});
    const AABB nearBounds{.Min = vec3(-60.0f, -60.0f, -80.0f), .Max = vec3(60.0f, 60.0f, -20.0f)};
    const PackedSceneLights nearPacked = PackSceneLights(*near, true, 1024, nearBounds);
    CHECK(nearPacked.PunctualCount == 1);
    CHECK(nearPacked.CascadeSetCount == 0);
}

TEST_CASE("PackSceneLights: the punctual depth bias is texel-scaled and clamped")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const Unique<Scene> scene = Scene::Create(types);

    // A huge range at a fine resolution drives worldPerTexel*0.5 past the clamp ceiling.
    AddLight(*scene, Light{.Type = LightType::Spot, .Range = 10000.0f, .OuterCone = 0.6f});
    // A tiny range drives it below the clamp floor.
    AddLight(*scene, Light{.Type = LightType::Spot, .Range = 0.001f, .OuterCone = 0.6f});

    const PackedSceneLights packed = PackSceneLights(*scene, true, 1024);

    REQUIRE(packed.PunctualCount == 2);
    CHECK(packed.PunctualRecords[0].Params.w == doctest::Approx(0.01f));   // clamped to ceiling
    CHECK(packed.PunctualRecords[1].Params.w == doctest::Approx(0.0005f)); // clamped to floor
}

TEST_CASE("PackSceneLights: a light that declines shadows takes no slot and yields it to the next")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const Unique<Scene> scene = Scene::Create(types);

    // Exactly as many casters as there are slots, preceded by a light that wants none. The
    // declining light is *first*, so a skip that still moved the counter would starve the last
    // caster — which is the failure this exists to prevent, and it is invisible in a picture.
    AddLight(*scene, Light{.Type = LightType::Point, .Range = 5.0f, .CastsShadows = false});
    for (u32 i = 0; i < MaxShadowedPunctual; ++i)
    {
        AddLight(*scene, Light{.Type = LightType::Point, .Range = 5.0f},
                 vec3(static_cast<f32>(i + 1), 0.0f, 0.0f));
    }

    const PackedSceneLights packed = PackSceneLights(*scene, true, 1024);

    REQUIRE(packed.LightCount == MaxShadowedPunctual + 1);
    CHECK(packed.PunctualCount == MaxShadowedPunctual);

    // The decliner is unshadowed and every caster behind it is slotted.
    CHECK(packed.Lights[0].Cone.z < 0.0f);
    for (u32 i = 1; i < packed.LightCount; ++i)
    {
        CHECK(packed.Lights[i].Cone.z >= 0.0f);
    }
}

TEST_CASE("PackSceneLights: a non-casting area light is not selected to drive the cascade")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const Unique<Scene> scene = Scene::Create(types);

    // The same geometry the cascade route keys on above — a small bound subtending a tiny angle
    // from a distant light — with the light declining shadows.
    AddLight(*scene, Light{.Type = LightType::Sphere,
                           .Direction = vec3(0.0f, 0.0f, -1.0f),
                           .Range = 10000.0f,
                           .Radius = 50.0f,
                           .CastsShadows = false});
    const AABB farBounds{.Min = vec3(-2.0f, -2.0f, -102.0f), .Max = vec3(2.0f, 2.0f, -98.0f)};

    const PackedSceneLights packed = PackSceneLights(*scene, true, 1024, farBounds);

    REQUIRE(packed.LightCount == 1);
    // Neither arm claims it: no cascade set, and no punctual tile either.
    CHECK(packed.CascadeSetCount == 0);
    CHECK(packed.PunctualCount == 0);
    CHECK(packed.Lights[0].Cone.z < 0.0f);
    // And the shader is told it is not cascade-shadowed, so it shades unshadowed rather than
    // sampling a cascade fit to some other light's direction.
    CHECK_FALSE(IsAreaCascadeShadowed(packed.Lights[0]));
}

TEST_CASE("PackSceneLights: a Sphere's lighting radius and its shadow source radius are two lanes")
{
    TypeRegistry types;
    RegisterBuiltins(types);

    // Area.x feeds the LTC area-lighting integral, which is exact at any emitter size; AreaNormal.w
    // feeds the PCSS shadow estimator, which is an approximation the lighting pass caps by angular
    // size per fragment. The pack writes the authored world radius into both, uncapped: a cap here
    // would have no distance to be angular against, and would silently dim and harden the light it
    // was meant to leave alone. A source far larger than anything it could shadow is the case that
    // would tempt one.
    for (const f32 radius : {0.25f, 50.0f, 1.0e6f})
    {
        const Unique<Scene> scene = Scene::Create(types);
        AddLight(*scene, Light{.Type = LightType::Sphere, .Range = 10.0f, .Radius = radius});
        const PackedSceneLights packed = PackSceneLights(*scene, true, 1024);
        REQUIRE(packed.LightCount == 1);
        CHECK(packed.Lights[0].Area.x == doctest::Approx(radius));
        CHECK(packed.Lights[0].AreaNormal.w == doctest::Approx(radius));
    }

    // A punctual light's Radius is a shading-distance clamp, not an emitter, so it sizes no
    // penumbra: the shadow lane stays zero and that light's PCSS widths are the narrowest there
    // are, whatever the authored radius.
    const Unique<Scene> spot = Scene::Create(types);
    AddLight(*spot, Light{.Type = LightType::Spot, .Range = 10.0f, .Radius = 2.0f});
    const PackedSceneLights spotPacked = PackSceneLights(*spot, true, 1024);
    REQUIRE(spotPacked.LightCount == 1);
    CHECK(spotPacked.Lights[0].Area.x == doctest::Approx(2.0f));
    CHECK(spotPacked.Lights[0].AreaNormal.w == 0.0f);
}

TEST_CASE("PackSceneLights: a punctual slot and a cube face go only where the camera can see")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const Unique<Scene> scene = Scene::Create(types);

    // A 90° camera at the origin looking down -Z: at z = -10 the frustum spans x in [-10, 10].
    CameraView camera;
    camera.SetPerspective(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f), vec3(0.0f, 0.0f, -1.0f), vec3(0.0f, 1.0f, 0.0f));
    const Frustum frustum = Frustum::FromViewProjection(camera.ViewProjection());

    // Iterated first, so without the frustum it would take slot 0: it sits 20 units right of the
    // frustum's edge, about 14 from its side plane, and reaches 3.
    AddLight(*scene, Light{.Type = LightType::Point, .Range = 3.0f}, vec3(30.0f, 0.0f, -10.0f));
    // 2 units past the same edge, about 1.4 from the plane, so its range straddles it.
    AddLight(*scene, Light{.Type = LightType::Point, .Range = 3.0f}, vec3(12.0f, 0.0f, -10.0f));

    const PackedSceneLights packed = PackSceneLights(*scene, true, 1024, AABB::Empty(), &frustum);

    // The light out of the frustum's reach lights no visible pixel, so it is not packed at all.
    REQUIRE(packed.LightCount == 1);
    REQUIRE(packed.PunctualCount == 1);
    CHECK(packed.Lights[0].PositionRange.x == doctest::Approx(12.0f));
    CHECK(packed.Lights[0].Cone.z == doctest::Approx(0.0f));

    // CubeFace order is +X, -X, +Y, -Y, +Z, -Z. The +X face looks away from the frustum and is
    // skipped; the -X face looks into it and is rendered.
    const u8 mask = packed.PunctualFaceMask[0];
    CHECK((mask & (1u << 0)) == 0);
    CHECK((mask & (1u << 1)) != 0);

    // Without a camera frustum both lights are packed, both take a slot, and every face renders.
    const PackedSceneLights untested = PackSceneLights(*scene, true, 1024);
    REQUIRE(untested.LightCount == 2);
    REQUIRE(untested.PunctualCount == 2);
    CHECK(untested.PunctualFaceMask[0] == 0x3F);
    CHECK(untested.PunctualFaceMask[1] == 0x3F);
}

TEST_CASE("PackSceneLights: a light that cannot contribute is not packed")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const Unique<Scene> scene = Scene::Create(types);

    // Each of these adds exactly nothing to any pixel, so each would only spend a slot.
    AddLight(*scene, Light{.Type = LightType::Point, .Intensity = 0.0f});
    AddLight(*scene, Light{.Type = LightType::Spot, .Color = vec3(0.0f)});
    AddLight(*scene, Light{.Type = LightType::Point, .Range = 0.0f});
    AddLight(*scene, Light{.Type = LightType::Rect, .Width = 0.0f});
    AddLight(*scene, Light{.Type = LightType::Sphere, .Radius = 0.0f});
    AddLight(*scene, Light{.Type = LightType::Polygon,
                           .PolygonVertices = {vec3(0.0f), vec3(1.0f, 0.0f, 0.0f),
                                               vec3(2.0f, 0.0f, 0.0f)}});

    // A directional has no range, so a zero one does not stop it lighting everything.
    AddLight(*scene, Light{.Type = LightType::Directional, .Range = 0.0f});

    const PackedSceneLights packed = PackSceneLights(*scene, true, 1024);

    REQUIRE(packed.LightCount == 1);
    CHECK(packed.Lights[0].DirectionType.w == doctest::Approx(0.0f)); // LightType::Directional
    CHECK(packed.DroppedLightCount == 0);
}

TEST_CASE("PackSceneLights: past the cap, the nearest bright light is packed and a dark one never")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const Unique<Scene> scene = Scene::Create(types);

    const vec3 viewpoint(0.0f);
    // A dark light at the viewpoint, iterated first: nearest of all, and contributing nothing.
    AddLight(*scene, Light{.Type = LightType::Point, .Intensity = 0.0f, .Range = 100.0f},
             viewpoint);
    // Four more bright lights than the cap, iterated farthest first, so the nearest comes last
    // and a pack that kept the first MaxLights would drop it.
    const u32 bright = Renderer::SceneView::MaxLights + 4;
    for (u32 i = 0; i < bright; ++i)
    {
        AddLight(*scene, Light{.Type = LightType::Point, .Intensity = 1000.0f, .Range = 100.0f},
                 vec3(static_cast<f32>(2 * (bright - i)), 0.0f, 0.0f));
    }

    const PackedSceneLights packed =
        PackSceneLights(*scene, true, 1024, AABB::Empty(), nullptr, &viewpoint);

    REQUIRE(packed.LightCount == Renderer::SceneView::MaxLights);
    CHECK(packed.DroppedLightCount == bright - Renderer::SceneView::MaxLights);

    f32 dimmestPacked = std::numeric_limits<f32>::max();
    f32 farthestPacked = 0.0f;
    bool nearestPacked = false;
    for (u32 i = 0; i < packed.LightCount; ++i)
    {
        const PackedLight& light = packed.Lights[i];
        dimmestPacked = std::min(dimmestPacked, light.ColorIntensity.a);
        farthestPacked = std::max(farthestPacked, light.PositionRange.x);
        nearestPacked = nearestPacked || light.PositionRange.x == 2.0f;
    }
    CHECK(dimmestPacked > 0.0f);
    CHECK(nearestPacked);
    // The packed set is the nearest MaxLights, not merely one that contains the nearest.
    CHECK(farthestPacked == doctest::Approx(2.0f * Renderer::SceneView::MaxLights));
}
