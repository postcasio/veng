#include "SkySourceResolve.h"

#include <Veng/Renderer/SceneView.h>
#include <Veng/Scene/Components.h>

namespace Veng::Renderer
{
    ResolvedSkySource ResolveSkySource(const Sky* sky, SceneView& view)
    {
        ResolvedSkySource resolved;
        if (sky == nullptr || !sky->Source.HasValue())
        {
            return resolved;
        }

        resolved.Lighting = sky->Lighting;
        const TypeId active = sky->Source.ActiveType();
        const void* source = sky->Source.ActivePtr();
        if (active == TypeIdOf<EnvironmentSky>())
        {
            const auto* environment = static_cast<const EnvironmentSky*>(source);
            view.Environment = environment->Map;
            view.EnvironmentIntensity = sky->Intensity;
            resolved.Kind = SkySourceKind::Environment;
        }
        else if (active == TypeIdOf<AtmosphereSky>())
        {
            const auto* atmosphere = static_cast<const AtmosphereSky*>(source);
            view.Atmosphere = atmosphere->Params;
            view.AtmosphereEnabled = true;
            view.AtmosphereIntensity = sky->Intensity;
            view.SkylightIntensity = sky->Intensity;
            resolved.Kind = SkySourceKind::Atmosphere;
            // Baked renders the atmosphere into a radiance cube the skybox path samples; direct runs
            // it per pixel every frame. The two render the same sky; the author picks per the sky's
            // dynamics and the renderer honors it (no silent switch).
            resolved.Baked = atmosphere->Mode == SkyMode::Baked;
        }
        else if (active == TypeIdOf<MaterialSky>())
        {
            const auto* material = static_cast<const MaterialSky*>(source);
            view.SkyMaterial = material->Material;
            view.SkyBakeKey = material->BakeKey;
            view.EnvironmentIntensity = sky->Intensity;
            // A baked material sky lights via the SH tier, so Intensity scales its ambient exactly
            // as it scales the atmosphere's; setting only EnvironmentIntensity left the SH knob dead.
            view.SkylightIntensity = sky->Intensity;
            resolved.Kind = SkySourceKind::Material;
            // Baked runs the material into a radiance cube the skybox path samples; direct runs it
            // per pixel every frame. The two render the same sky; the author picks per the sky's
            // dynamics and the renderer honors it (no silent switch).
            resolved.Baked = material->Mode == SkyMode::Baked;
        }
        return resolved;
    }
}
