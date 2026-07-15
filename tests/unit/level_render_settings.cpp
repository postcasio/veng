// ApplyLevelRenderSettings — the single level→renderer post/pipeline mapping the example games, the
// managed world, and the editor viewport share. Pure struct copy, no device: these check the
// auto-exposure metering knobs land on the ViewState, and that a level authoring none keeps the
// engine's ViewState defaults. Runs ICD-free.

#include <doctest/doctest.h>

#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Viewport.h>
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
