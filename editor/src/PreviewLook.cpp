#include "PreviewLook.h"

#include <Veng/Asset/Environment.h>
#include <Veng/Scene/Scene.h>

#include "EditorOnly.h"

namespace VengEditor
{
    using namespace Veng;

    PreviewLighting PlanPreviewLighting(const Scene& scene, bool environment)
    {
        bool hasLight = false;
        scene.Each<Light>([&hasLight](Entity, const Light&) { hasLight = true; });
        bool hasSky = false;
        scene.Each<Sky>([&hasSky](Entity, const Sky&) { hasSky = true; });

        const bool sky = environment && !hasSky;
        return {.Sky = sky, .Sun = !hasLight && !sky};
    }

    void AddPreviewLighting(Scene& scene, const AssetHandle<EnvironmentMap>& environment)
    {
        const PreviewLighting plan = PlanPreviewLighting(scene, environment.IsValid());
        if (plan.Sky)
        {
            const Entity entity = scene.CreateEntity();
            scene.Add<EditorOnly>(entity);
            scene.Add<Name>(entity) = Name{.Value = "Preview Sky"};
            Sky& sky = scene.Add<Sky>(entity);
            sky.Lighting = SkyLighting::IBL;
            auto* source =
                static_cast<EnvironmentSky*>(sky.Source.SetActive(TypeIdOf<EnvironmentSky>()));
            source->Map = environment;
        }
        if (plan.Sun)
        {
            const Entity entity = scene.CreateEntity();
            scene.Add<EditorOnly>(entity);
            scene.Add<Name>(entity) = Name{.Value = "Preview Light"};
            scene.Add<Light>(entity) = Light{
                .Type = LightType::Directional,
                .Direction = glm::normalize(vec3(-0.4f, -0.7f, -0.5f)),
                .Color = vec3(1.0f, 1.0f, 1.0f),
                // A directional's intensity is an illuminance in lux: direct daylight.
                .Intensity = 100000.0f,
            };
        }
    }
}
