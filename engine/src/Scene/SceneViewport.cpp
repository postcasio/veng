#include <Veng/Scene/SceneViewport.h>

#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>

namespace Veng
{
    void PushSceneView(Renderer::Viewport& viewport, const Scene& scene,
                       const Renderer::ViewState& knobs, const f32 delta)
    {
        const Ref<Renderer::ImageView> output = viewport.GetOutput();
        const f32 aspect = static_cast<f32>(output->GetImage()->GetWidth()) /
                           static_cast<f32>(output->GetImage()->GetHeight());

        Renderer::ViewState state = knobs;
        state.World = &scene;
        state.Camera = ResolvePrimaryCameraView(scene, aspect).value_or(DefaultCameraView(aspect));
        state.Delta = delta;
        viewport.SetViewState(state);
    }

    void ApplyLevelRenderSettings(const LevelRenderSettings& render,
                                  Renderer::SceneRendererSettings& settings,
                                  Renderer::ViewState& view)
    {
        settings.Bloom = render.Bloom;
        settings.Shadows = render.Shadows;
        settings.AO = render.AO;
        settings.Skybox = render.Skybox;

        view.Exposure = render.Exposure;
        view.BloomIntensity = render.BloomIntensity;
        view.Environment = render.Environment;
        view.EnvironmentIntensity = render.EnvironmentIntensity;
    }
}
