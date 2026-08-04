#include <Veng/Scene/SceneViewport.h>

#include <algorithm>

#include <Veng/Renderer/DofTile.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

namespace Veng
{
    void PushSceneView(Renderer::Viewport& viewport, const Scene& scene,
                       const Renderer::ViewState& knobs, const f32 delta, const f32 alpha)
    {
        const Ref<Renderer::ImageView> output = viewport.GetOutput();
        const f32 aspect = static_cast<f32>(output->GetImage()->GetWidth()) /
                           static_cast<f32>(output->GetImage()->GetHeight());

        Renderer::ViewState state = knobs;
        state.World = &scene;
        state.Camera = ResolvePrimaryCameraView(scene, aspect).value_or(DefaultCameraView(aspect));
        state.Delta = delta;
        state.Alpha = alpha;
        ResolveDofViewState(state, static_cast<f32>(output->GetImage()->GetHeight()));
        viewport.SetViewState(state);
    }

    void ResolveDofViewState(Renderer::ViewState& state, const f32 viewportPixelHeight)
    {
        // A camera that is not Physical authors no sensor, so the scale falls back to the same
        // full-frame reference height the Camera component defaults to.
        constexpr f32 DefaultSensorHeightMetres = 0.024f;

        const optional<CameraLens>& lens = state.Camera.GetLens();
        state.DofFromPhysicalCamera = lens.has_value();
        if (lens.has_value())
        {
            const DofParams params = ComputeDofParams(*lens, viewportPixelHeight);
            state.DofFocusDistance = params.FocusDistance;
            state.DofAperture = params.Aperture;
            state.DofCocScale = params.CocScale;
        }
        else
        {
            state.DofCocScale = std::max(viewportPixelHeight, 1.0f) / DefaultSensorHeightMetres;
        }

        state.DofMaxCoc = Renderer::ClampDofMaxCoc(state.DofMaxCoc);
        state.DofRingCount = Renderer::ClampDofRingCount(state.DofRingCount);
    }

    void ApplyLevelRenderSettings(const LevelRenderSettings& render,
                                  Renderer::SceneRendererSettings& settings,
                                  Renderer::ViewState& view)
    {
        settings.Bloom = render.Bloom;
        settings.Shadows = render.Shadows;
        settings.PunctualShadows = render.PunctualShadows;
        settings.MaxShadowDistance = render.MaxShadowDistance;
        settings.ShadowResolution = render.ShadowResolution;
        settings.AutoExposure = render.AutoExposure;
        settings.SSR = render.SSR;
        settings.AO = render.AO;
        settings.Refraction = render.Refraction;
        settings.DepthOfField = render.DepthOfField;

        view.AmbientFloor = render.AmbientFloor;
        view.Exposure = render.Exposure;
        view.Tonemapper = render.Tonemapper;
        view.AutoExposureMaxLuminance = render.AutoExposureMaxLuminance;
        view.AutoExposureLowPercentile = render.AutoExposureLowPercentile;
        view.AutoExposureHighPercentile = render.AutoExposureHighPercentile;
        view.BloomThreshold = render.BloomThreshold;
        view.BloomIntensity = render.BloomIntensity;
        view.BloomRadius = render.BloomRadius;

        // An unconditional mapping: the authored focus and aperture are recorded even while a
        // Physical camera overwrites them on every push, so they come back the moment it stops
        // being Physical without the level being reloaded. The two quality knobs are clamped here
        // as well as at the push, because a cooked level is untrusted input.
        view.DofFocusDistance = render.DofFocusDistance;
        view.DofAperture = render.DofAperture;
        view.DofMaxCoc = Renderer::ClampDofMaxCoc(render.DofMaxCoc);
        view.DofRingCount = Renderer::ClampDofRingCount(render.DofRingCount);
    }
}
