// ApplyRenderLook — the single look→renderer post/pipeline mapping a viewport's look resolve, a
// host's graphics resolve and the editor's previews share — and CopyLookKnobs, which carries a
// resolved look's per-frame half over each pushed view. Pure struct copies, no device: these check
// the auto-exposure metering knobs land on the ViewState, that a look leaving them unset keeps the
// engine's ViewState defaults, and that the per-frame carry writes what the mapping writes and
// nothing else. Runs ICD-free.

#include <doctest/doctest.h>

#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Renderer/DofTile.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/SceneViewport.h>

using namespace Veng;

TEST_CASE("ApplyRenderLook delivers the authored auto-exposure metering onto the ViewState")
{
    RenderLook render;
    render.AutoExposureMinLuminance = 0.0005f;
    render.AutoExposureMaxLuminance = 4000.0f;
    render.AutoExposureLowPercentile = 0.6f;
    render.AutoExposureHighPercentile = 0.98f;

    Renderer::SceneRendererSettings settings;
    Renderer::ViewState view;
    ApplyRenderLook(render, settings, view);

    CHECK(view.AutoExposureMinLuminance == doctest::Approx(0.0005f));
    CHECK(view.AutoExposureMaxLuminance == doctest::Approx(4000.0f));
    CHECK(view.AutoExposureLowPercentile == doctest::Approx(0.6f));
    CHECK(view.AutoExposureHighPercentile == doctest::Approx(0.98f));
}

TEST_CASE("A look authoring no metering keeps the engine ViewState defaults")
{
    // A default-constructed RenderLook carries the same metering values as the renderer's own
    // ViewState defaults, so a look that authors none renders as the renderer would.
    const RenderLook render; // engine defaults
    const Renderer::ViewState defaults;

    Renderer::SceneRendererSettings settings;
    Renderer::ViewState view;
    ApplyRenderLook(render, settings, view);

    CHECK(view.AutoExposureMinLuminance == doctest::Approx(defaults.AutoExposureMinLuminance));
    CHECK(view.AutoExposureMaxLuminance == doctest::Approx(defaults.AutoExposureMaxLuminance));
    CHECK(view.AutoExposureLowPercentile == doctest::Approx(defaults.AutoExposureLowPercentile));
    CHECK(view.AutoExposureHighPercentile == doctest::Approx(defaults.AutoExposureHighPercentile));
}

TEST_CASE("ApplyRenderLook clamps the authored depth-of-field quality knobs")
{
    RenderLook render;
    render.DepthOfField = true;
    render.DofMaxCoc = 4096.0f;
    render.DofRingCount = 4096;

    Renderer::SceneRendererSettings settings;
    Renderer::ViewState view;
    ApplyRenderLook(render, settings, view);

    CHECK(settings.DepthOfField);
    CHECK(view.DofMaxCoc == doctest::Approx(Renderer::DofCocCeiling));
    CHECK(view.DofRingCount == Renderer::MaxDofRings);
}

TEST_CASE("A Physical camera wins over the authored lens values without reapplying the look")
{
    // The look authors focus and aperture; the mapping is unconditional, so the values are recorded
    // whatever camera is active.
    RenderLook render;
    render.DepthOfField = true;
    render.DofFocusDistance = 3.5f;
    render.DofAperture = 0.05f;
    render.DofRingCount = 6;

    Renderer::SceneRendererSettings settings;
    Renderer::ViewState knobs;
    ApplyRenderLook(render, settings, knobs);
    CHECK(knobs.DofFocusDistance == doctest::Approx(3.5f));

    Camera physical;
    physical.Projection = CameraProjection::Physical;
    physical.FocusDistance = 12.0f;
    physical.FStop = 1.4f;

    // Frame one: a Physical camera is resolved, so its lens overwrites the per-frame copy — and
    // the ring count, a quality knob rather than a lens value, still comes from the look.
    Renderer::ViewState physicalFrame = knobs;
    physicalFrame.Camera = MakeCameraView(physical, 1.0f, mat4(1.0f));
    ResolveDofViewState(physicalFrame, 720.0f);
    CHECK(physicalFrame.DofFromPhysicalCamera);
    CHECK(physicalFrame.DofFocusDistance == doctest::Approx(12.0f));
    CHECK(physicalFrame.DofRingCount == 6);

    // Frame two: the same stored knobs, now pushed with a non-Physical camera. The authored focus
    // takes effect immediately — nothing was overwritten by the mapping, so nothing is reapplied.
    Camera perspective;
    perspective.Projection = CameraProjection::Perspective;

    Renderer::ViewState perspectiveFrame = knobs;
    perspectiveFrame.Camera = MakeCameraView(perspective, 1.0f, mat4(1.0f));
    ResolveDofViewState(perspectiveFrame, 720.0f);
    CHECK_FALSE(perspectiveFrame.DofFromPhysicalCamera);
    CHECK(perspectiveFrame.DofFocusDistance == doctest::Approx(3.5f));
    CHECK(perspectiveFrame.DofAperture == doctest::Approx(0.05f));
}

namespace
{
    // A look whose every per-frame field differs from the ViewState defaults.
    RenderLook DistinctLook()
    {
        RenderLook look;
        look.Exposure = 2.5f;
        look.Tonemapper = Renderer::Tonemapper::AgX;
        look.AutoExposureMinLuminance = 0.01f;
        look.AutoExposureMaxLuminance = 50.0f;
        look.AutoExposureLowPercentile = 0.2f;
        look.AutoExposureHighPercentile = 0.9f;
        look.BloomThreshold = 0.5f;
        look.BloomIntensity = 1.7f;
        look.BloomRadius = 2.0f;
        look.AmbientFloor = vec3(0.4f, 0.5f, 0.6f);
        look.DofFocusDistance = 4.0f;
        look.DofAperture = 0.03f;
        look.DofMaxCoc = 9.0f;
        look.DofRingCount = 3;
        return look;
    }
}

TEST_CASE("CopyLookKnobs carries exactly the per-frame fields the look mapping writes")
{
    const RenderLook look = DistinctLook();

    Renderer::SceneRendererSettings settings;
    Renderer::ViewState resolved;
    ApplyRenderLook(look, settings, resolved);

    // A pushed view whose knobs a look does not own are all off their defaults, so a carry that
    // wrote one of them would show.
    Renderer::ViewState pushed;
    pushed.SsrIntensity = 3.0f;
    pushed.AutoExposureKey = 0.4f;
    pushed.AutoExposureSpeed = 7.0f;
    pushed.OutputBrightness = 1.3f;
    pushed.OutputGamma = 0.9f;
    pushed.DofCocScale = 123.0f;
    pushed.Delta = 0.25f;

    Renderer::ViewState carried = pushed;
    CopyLookKnobs(resolved, carried);

    Renderer::ViewState direct = pushed;
    ApplyRenderLook(look, settings, direct);

    CHECK(carried.Exposure == direct.Exposure);
    CHECK(carried.Tonemapper == direct.Tonemapper);
    CHECK(carried.AutoExposureMinLuminance == direct.AutoExposureMinLuminance);
    CHECK(carried.AutoExposureMaxLuminance == direct.AutoExposureMaxLuminance);
    CHECK(carried.AutoExposureLowPercentile == direct.AutoExposureLowPercentile);
    CHECK(carried.AutoExposureHighPercentile == direct.AutoExposureHighPercentile);
    CHECK(carried.BloomThreshold == direct.BloomThreshold);
    CHECK(carried.BloomIntensity == direct.BloomIntensity);
    CHECK(carried.BloomRadius == direct.BloomRadius);
    CHECK(carried.AmbientFloor == direct.AmbientFloor);
    CHECK(carried.DofFocusDistance == direct.DofFocusDistance);
    CHECK(carried.DofAperture == direct.DofAperture);
    CHECK(carried.DofMaxCoc == direct.DofMaxCoc);
    CHECK(carried.DofRingCount == direct.DofRingCount);

    CHECK(carried.SsrIntensity == pushed.SsrIntensity);
    CHECK(carried.AutoExposureKey == pushed.AutoExposureKey);
    CHECK(carried.AutoExposureSpeed == pushed.AutoExposureSpeed);
    CHECK(carried.OutputBrightness == pushed.OutputBrightness);
    CHECK(carried.OutputGamma == pushed.OutputGamma);
    CHECK(carried.DofCocScale == pushed.DofCocScale);
    CHECK(carried.Delta == pushed.Delta);
}

TEST_CASE("CopyLookKnobs keeps a Physical camera's lens and clamps what a resolver wrote")
{
    Renderer::ViewState resolved;
    resolved.DofFocusDistance = 4.0f;
    resolved.DofAperture = 0.03f;
    resolved.DofMaxCoc = 4096.0f;
    resolved.DofRingCount = 4096;

    Renderer::ViewState pushed;
    pushed.DofFromPhysicalCamera = true;
    pushed.DofFocusDistance = 12.0f;
    pushed.DofAperture = 0.008f;
    CopyLookKnobs(resolved, pushed);

    CHECK(pushed.DofFocusDistance == doctest::Approx(12.0f));
    CHECK(pushed.DofAperture == doctest::Approx(0.008f));
    CHECK(pushed.DofMaxCoc == doctest::Approx(Renderer::DofCocCeiling));
    CHECK(pushed.DofRingCount == Renderer::MaxDofRings);
}
