#include <Veng/Scene/TimeOfDay.h>

#include <Veng/Renderer/SunPosition.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

namespace Veng
{
    void TimeOfDaySystem::DriveSun(Scene& scene)
    {
        const TimeOfDay* time = scene.TryGetFirst<TimeOfDay>();
        if (time == nullptr)
        {
            return;
        }

        const vec3 towardSun =
            Renderer::ComputeSunDirection(time->Orbit, time->Hours, time->DayOfYear);

        // Overwrite the first directional light's travel direction (the negated toward-sun
        // vector — a sun overhead travels down), so direct lighting, shadows, and the sky
        // that reads the same light all track the one derived sun.
        for (auto [entity, light] : scene.View<Light>())
        {
            if (light.Type == LightType::Directional)
            {
                light.Direction = -towardSun;
                break;
            }
        }
    }

    void TimeOfDaySystem::OnStart(Scene& scene, const SystemContext&)
    {
        DriveSun(scene);
    }

    void TimeOfDaySystem::OnUpdate(Scene& scene, f32, const SystemContext&)
    {
        DriveSun(scene);
    }
}
