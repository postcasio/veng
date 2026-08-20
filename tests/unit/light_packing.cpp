// Light-packing unit cases. PackSceneLights is the device-free CPU core of
// SceneRenderer's per-frame lighting setup (Execute calls it, then uploads the
// result): cascade-set and punctual shadow-slot assignment by estimated contribution,
// cone-cosine packing, and the std430 light layout. Pure scene-query + glm math — no
// Context, no driver — so the branchy slot/cap/record logic a golden image only
// exercises incidentally is testable here.

#include <doctest/doctest.h>

#include <cmath>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/LightPacking.h>
#include <Veng/Renderer/SceneRenderer.h>
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

TEST_CASE("PackSceneLights: a lone directional packs exactly as it did before cascade sets")
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

    // The overwhelmingly common scene — one shadow-casting directional — packs the identical
    // GpuLight it always has: set 0 is the flags word's zero, and the punctual slot stays -1
    // because a directional never takes an atlas tile. The ranking changed which light gets an
    // arm, not what a light looks like once it has one.
    const PackedLight& light = packed.Lights[0];
    CHECK(light.DirectionType.w == doctest::Approx(0.0f)); // LightType::Directional
    CHECK(light.ColorIntensity.a == doctest::Approx(2.0f));
    CHECK(light.Cone.z == doctest::Approx(-1.0f));
    CHECK(light.Cone.w == doctest::Approx(0.0f));
    CHECK(light.Area.w == doctest::Approx(-1.0f));
    CHECK(light.AreaNormal.w == doctest::Approx(0.0f));
    CHECK(packed.PunctualCount == 0);
    CHECK(packed.DeniedDirectionalCount == 0);
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
