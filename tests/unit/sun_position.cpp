// Time-of-day sun positioning: the SunOrbit solar math is pure glm — declination from the
// day of year, the toward-sun direction from hour + latitude — and ApplySceneSky's
// TimeOfDay resolution derives the view's sun and writes the directional light from it.
// No Context, no Vulkan symbol touched.

#include <doctest/doctest.h>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/SunPosition.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneViewport.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    bool VecApprox(const vec3& a, const vec3& b, f32 eps = 1e-4f)
    {
        return glm::all(glm::lessThan(glm::abs(a - b), vec3(eps)));
    }
}

TEST_CASE("Declination is zero at the equinoxes and peaks at the solstices")
{
    const SunOrbit orbit;
    CHECK(ComputeSunDeclination(orbit, 0.0f) == doctest::Approx(0.0f));
    CHECK(ComputeSunDeclination(orbit, orbit.YearLength * 0.5f) ==
          doctest::Approx(0.0f).epsilon(0.001));
    // Northern summer solstice reaches the full tilt, winter its negative.
    CHECK(ComputeSunDeclination(orbit, orbit.YearLength * 0.25f) ==
          doctest::Approx(glm::radians(orbit.AxialTilt)));
    CHECK(ComputeSunDeclination(orbit, orbit.YearLength * 0.75f) ==
          doctest::Approx(-glm::radians(orbit.AxialTilt)));
    // The day wraps over the year length.
    CHECK(ComputeSunDeclination(orbit, orbit.YearLength * 1.25f) ==
          doctest::Approx(glm::radians(orbit.AxialTilt)));

    // Zero tilt gives identical days year-round.
    SunOrbit flat;
    flat.AxialTilt = 0.0f;
    CHECK(ComputeSunDeclination(flat, 100.0f) == doctest::Approx(0.0f));
}

TEST_CASE("Equatorial equinox day: overhead noon, east sunrise, west sunset, midnight below")
{
    SunOrbit orbit;
    orbit.Latitude = 0.0f;

    CHECK(VecApprox(ComputeSunDirection(orbit, 12.0f), vec3(0.0f, 1.0f, 0.0f)));
    CHECK(VecApprox(ComputeSunDirection(orbit, 6.0f), vec3(1.0f, 0.0f, 0.0f)));
    CHECK(VecApprox(ComputeSunDirection(orbit, 18.0f), vec3(-1.0f, 0.0f, 0.0f)));
    CHECK(VecApprox(ComputeSunDirection(orbit, 0.0f), vec3(0.0f, -1.0f, 0.0f)));
    // Hours wrap: 36 is the same solar time as 12.
    CHECK(VecApprox(ComputeSunDirection(orbit, 36.0f), ComputeSunDirection(orbit, 12.0f)));
}

TEST_CASE("Northern-latitude noon sun sits south of overhead at the co-latitude elevation")
{
    const SunOrbit orbit; // Latitude 45, equinox.
    const vec3 noon = ComputeSunDirection(orbit, 12.0f);

    // Elevation is 90 - latitude + declination = 45 degrees at an equinox; south is +Z
    // (north is -Z), and the noon sun crosses the meridian (no east component).
    CHECK(noon.y == doctest::Approx(std::sin(glm::radians(45.0f))));
    CHECK(noon.z == doctest::Approx(std::cos(glm::radians(45.0f))));
    CHECK(noon.x == doctest::Approx(0.0f));

    // The summer solstice lifts noon by the axial tilt: elevation cos(lat - declination).
    const vec3 solsticeNoon = ComputeSunDirection(orbit, 12.0f, orbit.YearLength * 0.25f);
    CHECK(solsticeNoon.y ==
          doctest::Approx(std::cos(glm::radians(45.0f - orbit.AxialTilt))).epsilon(0.001));
    CHECK(solsticeNoon.y > noon.y);
}

TEST_CASE("Sun directions are unit length across the day and year")
{
    SunOrbit orbit;
    orbit.Latitude = 62.0f;
    for (const f32 day : {0.0f, 91.3f, 182.6f, 273.9f})
    {
        for (f32 hour = 0.0f; hour < 24.0f; hour += 1.5f)
        {
            CHECK(glm::length(ComputeSunDirection(orbit, hour, day)) == doctest::Approx(1.0f));
        }
    }
}

TEST_CASE("NorthHeading spins the sun path about world up")
{
    const SunOrbit orbit; // Latitude 45: the noon sun has a horizontal (southward) component.
    SunOrbit spun = orbit;
    spun.NorthHeading = 90.0f; // North is +X, so south (the noon side) is -X.

    const vec3 noon = ComputeSunDirection(spun, 12.0f);
    CHECK(noon.x == doctest::Approx(-std::cos(glm::radians(45.0f))));
    CHECK(noon.y == doctest::Approx(std::sin(glm::radians(45.0f))));
    CHECK(noon.z == doctest::Approx(0.0f));
}

TEST_CASE("ApplySceneSky derives the sun from TimeOfDay and writes the directional light")
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity lightEntity = scene->CreateEntity();
    Light light;
    light.Type = LightType::Directional;
    light.Direction = vec3(0.0f, -1.0f, 0.0f);
    scene->Add<Light>(lightEntity, light);

    TimeOfDay time;
    time.Hours = 9.0f;
    time.DayOfYear = 40.0f;
    scene->Add<TimeOfDay>(scene->CreateEntity(), time);

    SceneRendererSettings settings;
    ViewState view;
    ApplySceneSky(*scene, settings, view);

    const vec3 expected = ComputeSunDirection(time.Orbit, time.Hours, time.DayOfYear);
    CHECK(VecApprox(view.SunDirection, expected));
    // The directional light's travel direction is written from the derived sun, so direct
    // lighting and shadows track it.
    CHECK(VecApprox(scene->Get<Light>(lightEntity).Direction, -expected));
}

TEST_CASE("ApplySceneSky without TimeOfDay keeps the light-authored sun")
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity lightEntity = scene->CreateEntity();
    Light light;
    light.Type = LightType::Directional;
    light.Direction = glm::normalize(vec3(1.0f, -1.0f, 0.0f));
    scene->Add<Light>(lightEntity, light);

    SceneRendererSettings settings;
    ViewState view;
    ApplySceneSky(*scene, settings, view);

    CHECK(VecApprox(view.SunDirection, -scene->Get<Light>(lightEntity).Direction));
}

TEST_CASE("ApplySceneSky with TimeOfDay but no directional light still derives the view sun")
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    const Unique<Scene> scene = Scene::Create(registry);

    TimeOfDay time;
    time.Hours = 15.0f;
    scene->Add<TimeOfDay>(scene->CreateEntity(), time);

    SceneRendererSettings settings;
    ViewState view;
    ApplySceneSky(*scene, settings, view);

    CHECK(
        VecApprox(view.SunDirection, ComputeSunDirection(time.Orbit, time.Hours, time.DayOfYear)));
}
