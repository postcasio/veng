// Light-packing unit cases. PackSceneLights is the device-free CPU core of
// SceneRenderer's per-frame lighting setup (Execute calls it, then uploads the
// result): directional selection, punctual shadow-slot assignment, cone-cosine
// packing, and the std430 light layout. Pure scene-query + glm math — no Context,
// no driver — so the branchy slot/cap/record logic a golden image only exercises
// incidentally is testable here.

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
    CHECK_FALSE(packed.HaveDirectional);
    // The default travel is straight down, so a scene with no light still drives a
    // sensible directional matrix.
    CHECK(packed.DirectionalTravel == vec3{0.0f, -1.0f, 0.0f});
}

TEST_CASE("PackSceneLights: a directional light is selected and packed, never shadow-slotted")
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
    CHECK(packed.HaveDirectional);
    CHECK(packed.DirectionalTravel == vec3{0.3f, -0.8f, 0.5f});

    // DirectionType.w carries the LightType (0 == Directional); ColorIntensity.a the intensity.
    CHECK(packed.Lights[0].DirectionType.w == doctest::Approx(0.0f));
    CHECK(packed.Lights[0].ColorIntensity.a == doctest::Approx(2.0f));
    // A directional light is never assigned a punctual shadow slot (Cone.z stays -1).
    CHECK(packed.Lights[0].Cone.z == doctest::Approx(-1.0f));
    CHECK(packed.PunctualCount == 0);
}

TEST_CASE("PackSceneLights: only the first directional drives the directional travel")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const Unique<Scene> scene = Scene::Create(types);

    AddLight(*scene, Light{.Type = LightType::Directional, .Direction = vec3(1.0f, 0.0f, 0.0f)});
    AddLight(*scene, Light{.Type = LightType::Directional, .Direction = vec3(0.0f, 0.0f, 1.0f)});

    const PackedSceneLights packed = PackSceneLights(*scene, true, 1024);

    REQUIRE(packed.LightCount == 2);
    CHECK(packed.HaveDirectional);
    // Iteration is dense insertion order, so the first directional wins.
    CHECK(packed.DirectionalTravel == vec3{1.0f, 0.0f, 0.0f});
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
