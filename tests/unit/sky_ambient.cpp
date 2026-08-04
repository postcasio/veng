// The deferred lighting pass's flat-fallback ambient is a per-scene authored value, and every
// SH-lighting sky source scales its ambient by Sky::Intensity. Two device-free properties:
//
//  - ResolveSkySource sets BOTH EnvironmentIntensity and SkylightIntensity from Sky::Intensity for
//    a baked material sky (as it does for the atmosphere) — the SH tier reads SkylightIntensity,
//    which stayed at its default 1.0 for a material sky before, so the knob was dead.
//  - A scene authoring no floor resolves the engine's flat-ambient default, and that default rides
//    both lighting push blocks (base and SSAO) at one 16-byte-aligned offset — so a missed twin
//    struct fails here rather than silently on one pipeline at the byte level.

#include <doctest/doctest.h>

#include <cstddef>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/SceneView.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include "Renderer/Passes/DeferredLightingScenePass.h"
#include "Renderer/SkySourceResolve.h"

using namespace Veng;
using namespace Veng::Renderer;

TEST_CASE("ResolveSkySource: a baked material sky scales the SH ambient by Sky::Intensity")
{
    TypeRegistry types;
    const Unique<Scene> scene = Scene::Create(types);
    const CameraView camera;
    Renderer::SceneView view{.World = *scene, .Camera = camera};

    Sky sky;
    sky.Intensity = 2.5f;
    sky.Lighting = SkyLighting::SH;
    auto* material = static_cast<MaterialSky*>(sky.Source.SetActive(TypeIdOf<MaterialSky>()));
    material->Mode = SkyMode::Baked;

    const ResolvedSkySource resolved = ResolveSkySource(&sky, view);

    CHECK(resolved.Kind == SkySourceKind::Material);
    CHECK(resolved.Lighting == SkyLighting::SH);
    CHECK(resolved.Baked);
    // Both intensities carry Sky::Intensity: the SH tier reads SkylightIntensity, and setting only
    // EnvironmentIntensity is what left the knob dead for a material sky.
    CHECK(view.EnvironmentIntensity == doctest::Approx(2.5f));
    CHECK(view.SkylightIntensity == doctest::Approx(2.5f));
}

TEST_CASE("ResolveSkySource: the atmosphere sets both intensities identically")
{
    TypeRegistry types;
    const Unique<Scene> scene = Scene::Create(types);
    const CameraView camera;
    Renderer::SceneView view{.World = *scene, .Camera = camera};

    Sky sky;
    sky.Intensity = 3.0f;
    sky.Lighting = SkyLighting::SH;
    sky.Source.SetActive(TypeIdOf<AtmosphereSky>());

    ResolveSkySource(&sky, view);

    CHECK(view.AtmosphereIntensity == doctest::Approx(3.0f));
    CHECK(view.SkylightIntensity == doctest::Approx(3.0f));
}

TEST_CASE("AmbientFloor: no authored floor resolves the engine default across both push blocks")
{
    // The engine default is the historical flat ambient, so a consumer authoring nothing — a fresh
    // view, and an unauthored LevelRenderSettings — is byte-for-byte unchanged.
    const vec3 engineDefault{0.12f, 0.13f, 0.16f};

    TypeRegistry types;
    const Unique<Scene> scene = Scene::Create(types);
    const CameraView camera;
    const Renderer::SceneView view{.World = *scene, .Camera = camera};
    CHECK(view.AmbientFloor == engineDefault);
    CHECK(LevelRenderSettings{}.AmbientFloor == engineDefault);

    // Both lighting push blocks carry the floor, at one 16-byte-aligned offset — the SSAO twin as
    // well as the base — so a struct missing the field fails here (and at compile time in the pass).
    LightingPushConstants base{};
    base.AmbientFloor = view.AmbientFloor;
    SsaoLightingPushConstants ssao{};
    ssao.AmbientFloor = view.AmbientFloor;
    CHECK(base.AmbientFloor == engineDefault);
    CHECK(ssao.AmbientFloor == engineDefault);
    CHECK(offsetof(LightingPushConstants, AmbientFloor) ==
          offsetof(SsaoLightingPushConstants, AmbientFloor));
    CHECK(offsetof(LightingPushConstants, AmbientFloor) % 16 == 0);
}
