// ApplyLevelRenderSettings — the single level→renderer post/pipeline mapping the example games, the
// managed world, and the editor viewport share. Pure struct copy, no device: these check the
// auto-exposure metering knobs land on the ViewState, and that a level authoring none keeps the
// engine's ViewState defaults. Runs ICD-free.

#include <doctest/doctest.h>

#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Renderer/DofTile.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/SceneViewport.h>

using namespace Veng;

TEST_CASE(
    "ApplyLevelRenderSettings delivers the authored auto-exposure metering onto the ViewState")
{
    LevelRenderSettings render;
    render.AutoExposureMaxLuminance = 4000.0f;
    render.AutoExposureLowPercentile = 0.6f;
    render.AutoExposureHighPercentile = 0.98f;

    Renderer::SceneRendererSettings settings;
    Renderer::ViewState view;
    ApplyLevelRenderSettings(render, settings, view);

    CHECK(view.AutoExposureMaxLuminance == doctest::Approx(4000.0f));
    CHECK(view.AutoExposureLowPercentile == doctest::Approx(0.6f));
    CHECK(view.AutoExposureHighPercentile == doctest::Approx(0.98f));
}

TEST_CASE("A level authoring no metering keeps the engine ViewState defaults")
{
    // A default-constructed LevelRenderSettings carries the same metering values as the renderer's
    // own ViewState defaults, so a level that authors none is unchanged from today.
    const LevelRenderSettings render; // engine defaults
    const Renderer::ViewState defaults;

    Renderer::SceneRendererSettings settings;
    Renderer::ViewState view;
    ApplyLevelRenderSettings(render, settings, view);

    CHECK(view.AutoExposureMaxLuminance == doctest::Approx(defaults.AutoExposureMaxLuminance));
    CHECK(view.AutoExposureLowPercentile == doctest::Approx(defaults.AutoExposureLowPercentile));
    CHECK(view.AutoExposureHighPercentile == doctest::Approx(defaults.AutoExposureHighPercentile));
}

TEST_CASE("ApplyLevelRenderSettings clamps the authored depth-of-field quality knobs")
{
    LevelRenderSettings render;
    render.DepthOfField = true;
    render.DofMaxCoc = 4096.0f;
    render.DofRingCount = 4096;

    Renderer::SceneRendererSettings settings;
    Renderer::ViewState view;
    ApplyLevelRenderSettings(render, settings, view);

    CHECK(settings.DepthOfField);
    CHECK(view.DofMaxCoc == doctest::Approx(Renderer::DofCocCeiling));
    CHECK(view.DofRingCount == Renderer::MaxDofRings);
}

TEST_CASE("A Physical camera wins over the authored lens values without reloading the level")
{
    // The level authors focus and aperture; the seed is unconditional, so the values are recorded
    // whatever camera is active.
    LevelRenderSettings render;
    render.DepthOfField = true;
    render.DofFocusDistance = 3.5f;
    render.DofAperture = 0.05f;
    render.DofRingCount = 6;

    Renderer::SceneRendererSettings settings;
    Renderer::ViewState knobs;
    ApplyLevelRenderSettings(render, settings, knobs);
    CHECK(knobs.DofFocusDistance == doctest::Approx(3.5f));

    Camera physical;
    physical.Projection = CameraProjection::Physical;
    physical.FocusDistance = 12.0f;
    physical.FStop = 1.4f;

    // Frame one: a Physical camera is resolved, so its lens overwrites the per-frame copy — and
    // the ring count, a quality knob rather than a lens value, still comes from the level.
    Renderer::ViewState physicalFrame = knobs;
    physicalFrame.Camera = MakeCameraView(physical, 1.0f, mat4(1.0f));
    ResolveDofViewState(physicalFrame, 720.0f);
    CHECK(physicalFrame.DofFromPhysicalCamera);
    CHECK(physicalFrame.DofFocusDistance == doctest::Approx(12.0f));
    CHECK(physicalFrame.DofRingCount == 6);

    // Frame two: the same stored knobs, now pushed with a non-Physical camera. The authored focus
    // takes effect immediately — nothing was overwritten at the seed, so no reload is involved.
    Camera perspective;
    perspective.Projection = CameraProjection::Perspective;

    Renderer::ViewState perspectiveFrame = knobs;
    perspectiveFrame.Camera = MakeCameraView(perspective, 1.0f, mat4(1.0f));
    ResolveDofViewState(perspectiveFrame, 720.0f);
    CHECK_FALSE(perspectiveFrame.DofFromPhysicalCamera);
    CHECK(perspectiveFrame.DofFocusDistance == doctest::Approx(3.5f));
    CHECK(perspectiveFrame.DofAperture == doctest::Approx(0.05f));
}
