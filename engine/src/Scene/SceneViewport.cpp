#include <Veng/Scene/SceneViewport.h>

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

        view.Exposure = render.Exposure;
        view.BloomIntensity = render.BloomIntensity;
    }

    void ApplySceneSky(const Scene& scene, Renderer::SceneRendererSettings& settings,
                       Renderer::ViewState& view)
    {
        // Environment component → image-based lighting + skybox. Absent clears both, so removing
        // it reverts to the flat-ambient fallback.
        if (const Environment* environment = scene.TryGetFirst<Environment>())
        {
            view.Environment = environment->Map;
            view.EnvironmentIntensity = environment->Intensity;
            settings.Skybox = environment->Skybox;
        }
        else
        {
            view.Environment = {};
            settings.Skybox = false;
        }

        // Atmosphere component → procedural sky pass. Its presence drives both the topology and the
        // per-frame enable; absent reverts to default params so a removed component leaves no sky.
        if (const Atmosphere* atmosphere = scene.TryGetFirst<Atmosphere>())
        {
            settings.Atmosphere = true;
            view.AtmosphereEnabled = true;
            view.AtmosphereIntensity = atmosphere->Intensity;
            view.Atmosphere = atmosphere->Params;
        }
        else
        {
            settings.Atmosphere = false;
            view.AtmosphereEnabled = false;
            view.Atmosphere = Renderer::Atmosphere{};
        }

        // Skylight component (authored on the directional light) → dynamic SH ambient. It projects
        // view.Atmosphere along the sun direction below, so it lights even without the visible sky.
        if (const Skylight* skylight = scene.TryGetFirst<Skylight>())
        {
            settings.Skylight = true;
            view.SkylightIntensity = skylight->Intensity;
        }
        else
        {
            settings.Skylight = false;
        }

        // The sky and the SH skylight share the scene's sun: the toward-sun direction is the inverse
        // of the first directional light's world-space travel direction (a sun overhead travels
        // down). No directional light leaves the default world-up sun.
        view.SunDirection = vec3(0.0f, 1.0f, 0.0f);
        for ([[maybe_unused]] auto [entity, light] : scene.View<Light>())
        {
            if (light.Type == LightType::Directional)
            {
                const f32 length = glm::length(light.Direction);
                if (length > 1e-4f)
                {
                    view.SunDirection = -light.Direction / length;
                }
                break;
            }
        }
    }
}
