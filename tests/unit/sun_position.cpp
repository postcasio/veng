// Time-of-day sun positioning: the SunOrbit solar math is pure glm — declination from the
// day of year, the toward-sun direction from hour + latitude — and TimeOfDaySystem derives
// the sun from a TimeOfDay component and writes the directional light from it.
// No Context, no Vulkan symbol touched.

#include <doctest/doctest.h>

#include <Veng/Reflection/JsonSerialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/SunPosition.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/TimeOfDay.h>

#include <nlohmann/json.hpp>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    bool VecApprox(const vec3& a, const vec3& b, f32 eps = 1e-4f)
    {
        return glm::all(glm::lessThan(glm::abs(a - b), vec3(eps)));
    }

    // A SystemContext TimeOfDaySystem never reads: it touches neither the Input nor the
    // AssetManager, so backing storage is never dereferenced.
    struct ContextStorage
    {
        alignas(16) unsigned char InputBytes[64]{};
        alignas(16) unsigned char TasksBytes[64]{};
        alignas(16) unsigned char AssetsBytes[64]{};

        SystemContext Make()
        {
            return SystemContext{
                .Assets = *reinterpret_cast<AssetManager*>(AssetsBytes),
                .Input = *reinterpret_cast<Input*>(InputBytes),
                .Tasks = *reinterpret_cast<TaskSystem*>(TasksBytes),
            };
        }
    };
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

TEST_CASE("Polar latitudes get polar night and midnight sun at the solstices")
{
    SunOrbit orbit;
    orbit.Latitude = 80.0f; // Inside the polar circle for Earth's tilt.

    // Northern winter solstice: the sun never clears the horizon.
    const f32 winter = orbit.YearLength * 0.75f;
    // Northern summer solstice: it never sets.
    const f32 summer = orbit.YearLength * 0.25f;
    for (f32 hour = 0.0f; hour < 24.0f; hour += 1.5f)
    {
        CHECK(ComputeSunDirection(orbit, hour, winter).y < 0.0f);
        CHECK(ComputeSunDirection(orbit, hour, summer).y > 0.0f);
    }
}

TEST_CASE("An extreme axial tilt keeps the declination and directions valid")
{
    SunOrbit orbit;
    orbit.AxialTilt = 97.8f; // A Uranus-like sideways spin axis.

    // asin-form declination saturates at +/- 90 degrees instead of overshooting.
    const f32 solstice = ComputeSunDeclination(orbit, orbit.YearLength * 0.25f);
    CHECK(solstice <= glm::half_pi<f32>() + 1e-4f);
    CHECK(solstice == doctest::Approx(std::asin(std::sin(glm::radians(97.8f)))));

    for (f32 hour = 0.0f; hour < 24.0f; hour += 3.0f)
    {
        CHECK(glm::length(ComputeSunDirection(orbit, hour, orbit.YearLength * 0.25f)) ==
              doctest::Approx(1.0f));
    }
}

TEST_CASE("TimeOfDay round-trips through the JSON reflection walker")
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    const JsonFieldHooks hooks{};
    const TypeInfo& info = registry.Info(registry.IdOf<TimeOfDay>());

    TimeOfDay src;
    src.Hours = 6.75f;
    src.DayOfYear = 200.5f;
    src.Orbit.AxialTilt = 25.19f;
    src.Orbit.Latitude = -33.9f;
    src.Orbit.YearLength = 687.0f;
    src.Orbit.NorthHeading = 135.0f;

    const nlohmann::json doc = JsonWriteFields(&src, info, registry, hooks);

    TimeOfDay dst;
    const VoidResult result = JsonReadFields(&dst, info, doc, registry, hooks);
    REQUIRE(result);
    CHECK(dst.Hours == src.Hours);
    CHECK(dst.DayOfYear == src.DayOfYear);
    CHECK(dst.Orbit.AxialTilt == src.Orbit.AxialTilt);
    CHECK(dst.Orbit.Latitude == src.Orbit.Latitude);
    CHECK(dst.Orbit.YearLength == src.Orbit.YearLength);
    CHECK(dst.Orbit.NorthHeading == src.Orbit.NorthHeading);
}

TEST_CASE("TimeOfDaySystem derives the sun from TimeOfDay and writes the directional light")
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

    ContextStorage storage;
    TimeOfDaySystem system;
    system.OnUpdate(*scene, 0.016f, storage.Make());

    const vec3 expected = ComputeSunDirection(time.Orbit, time.Hours, time.DayOfYear);
    // The directional light's travel direction is written from the derived sun (its negation),
    // so direct lighting and shadows track it.
    CHECK(VecApprox(scene->Get<Light>(lightEntity).Direction, -expected));
}

TEST_CASE("TimeOfDaySystem without TimeOfDay leaves the authored light untouched")
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity lightEntity = scene->CreateEntity();
    Light light;
    light.Type = LightType::Directional;
    light.Direction = glm::normalize(vec3(1.0f, -1.0f, 0.0f));
    scene->Add<Light>(lightEntity, light);

    ContextStorage storage;
    TimeOfDaySystem system;
    system.OnUpdate(*scene, 0.016f, storage.Make());

    CHECK(VecApprox(scene->Get<Light>(lightEntity).Direction,
                    glm::normalize(vec3(1.0f, -1.0f, 0.0f))));
}

TEST_CASE("TimeOfDaySystem writes the first directional light even behind other light types")
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    const Unique<Scene> scene = Scene::Create(registry);

    // A point light created first must be skipped by the directional-sun search and
    // left untouched by the TimeOfDay write.
    const Entity pointEntity = scene->CreateEntity();
    Light point;
    point.Type = LightType::Point;
    point.Direction = vec3(0.0f, -1.0f, 0.0f);
    scene->Add<Light>(pointEntity, point);

    const Entity sunEntity = scene->CreateEntity();
    Light sun;
    sun.Type = LightType::Directional;
    sun.Direction = vec3(0.0f, -1.0f, 0.0f);
    scene->Add<Light>(sunEntity, sun);

    TimeOfDay time;
    time.Hours = 16.0f;
    scene->Add<TimeOfDay>(scene->CreateEntity(), time);

    ContextStorage storage;
    TimeOfDaySystem system;
    system.OnUpdate(*scene, 0.016f, storage.Make());

    const vec3 expected = ComputeSunDirection(time.Orbit, time.Hours, time.DayOfYear);
    CHECK(VecApprox(scene->Get<Light>(sunEntity).Direction, -expected));
    CHECK(VecApprox(scene->Get<Light>(pointEntity).Direction, vec3(0.0f, -1.0f, 0.0f)));
}

TEST_CASE("TimeOfDaySystem with TimeOfDay but no directional light is a no-op")
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    const Unique<Scene> scene = Scene::Create(registry);

    TimeOfDay time;
    time.Hours = 15.0f;
    scene->Add<TimeOfDay>(scene->CreateEntity(), time);

    // No directional light to write; the system must run without touching anything.
    ContextStorage storage;
    TimeOfDaySystem system;
    system.OnUpdate(*scene, 0.016f, storage.Make());

    CHECK(scene->TryGetFirst<Light>() == nullptr);
}
