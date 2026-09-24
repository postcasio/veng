// The deferred lighting pass's flat-fallback ambient is a per-scene authored value, and every
// SH-lighting sky source scales its ambient by Sky::Intensity. Two device-free properties:
//
//  - ResolveSkySource sets BOTH EnvironmentIntensity and SkylightIntensity from Sky::Intensity for
//    a baked material sky (as it does for the atmosphere) — the SH tier reads SkylightIntensity,
//    which stayed at its default 1.0 for a material sky before, so the knob was dead.
//  - A scene authoring no floor resolves the engine's flat-ambient default, and that default rides
//    the view block's AmbientFloor at a 16-byte-aligned offset, where every lit surface — the
//    deferred pass and a forward-lit translucent alike — reads it.
//  - ResolveAmbientArm picks one arm per view: IBL only with its source resident, the SH skylight
//    only where IBL does not light, the flat floor otherwise.

#include <doctest/doctest.h>

#include <cstddef>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/SceneView.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include "Renderer/GpuBlocks.h"
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

TEST_CASE("AmbientFloor: no authored floor resolves the engine default into the view block")
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

    // The floor's rgb rides the view block's AmbientFloor vec4 at a std140/std430 16-byte boundary,
    // so the shader's float4 read lands on it.
    ViewConstantsBlock block{};
    block.AmbientFloor = vec4(view.AmbientFloor, view.EnvironmentIntensity);
    CHECK(vec3(block.AmbientFloor) == engineDefault);
    CHECK(offsetof(ViewConstantsBlock, AmbientFloor) % 16 == 0);
    CHECK(sizeof(ViewConstantsBlock) == BindlessRegistry::ViewConstantsStride);
}

TEST_CASE("ResolveAmbientArm: IBL needs its source resident, and SH lights only where IBL does not")
{
    // IBL wins whenever it is allowed and its cube-backed source is resident, whatever SH wants.
    CHECK(ResolveAmbientArm(true, true, false) == AmbientArm::Ibl);
    CHECK(ResolveAmbientArm(true, true, true) == AmbientArm::Ibl);
    // An allowed IBL tier whose source is not resident falls through, never to a half-lit IBL.
    CHECK(ResolveAmbientArm(true, false, true) == AmbientArm::Skylight);
    CHECK(ResolveAmbientArm(true, false, false) == AmbientArm::Flat);
    // Residency alone does not light the scene through IBL.
    CHECK(ResolveAmbientArm(false, true, true) == AmbientArm::Skylight);
    CHECK(ResolveAmbientArm(false, true, false) == AmbientArm::Flat);
}
