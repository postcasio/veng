#include "PreviewLook.h"

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

    void AddPreviewLighting(Scene& scene)
    {
        if (!PlanPreviewLighting(scene, false).Sun)
        {
            return;
        }
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

    SceneLighting DefaultSceneLighting(const Scene& scene, AssetId environment)
    {
        const PreviewLighting plan = PlanPreviewLighting(scene, environment.IsValid());
        SceneLighting lighting;
        lighting.Environment = plan.Sky ? environment : AssetId{};
        lighting.Sun = plan.Sun;
        lighting.SunIntensity = plan.Sun ? 10.0f : 3.0f;
        return lighting;
    }
}
