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
        // An atmosphere is defined by the frame's own up and the scene's sun, so it does not turn;
        // every other source is a sky with a frame of its own, oriented by Rotation.
        const f32 rotationLength = glm::length(sky->Rotation);
        const quat orientation = rotationLength > 1.0e-6f ? sky->Rotation / rotationLength
                                                          : quat(1.0f, 0.0f, 0.0f, 0.0f);
        const TypeId active = sky->Source.ActiveType();
        const void* source = sky->Source.ActivePtr();
        if (active == TypeIdOf<EnvironmentSky>())
        {
            const auto* environment = static_cast<const EnvironmentSky*>(source);
            view.Environment = environment->Map;
            view.EnvironmentIntensity = sky->Intensity;
            resolved.Kind = SkySourceKind::Environment;
            view.SkyRotation = orientation;
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
            view.EnvironmentIntensity = sky->Intensity;
            // A baked material sky lights via the SH tier, so Intensity scales its ambient exactly
            // as it scales the atmosphere's; setting only EnvironmentIntensity left the SH knob dead.
            view.SkylightIntensity = sky->Intensity;
            resolved.Kind = SkySourceKind::Material;
            view.SkyRotation = orientation;
            // Baked runs the material into a radiance cube the skybox path samples; direct runs it
            // per pixel every frame. The two render the same sky; the author picks per the sky's
            // dynamics and the renderer honors it (no silent switch).
            resolved.Baked = material->Mode == SkyMode::Baked;
        }
        else if (active == TypeIdOf<CubeSky>())
        {
            const auto* cube = static_cast<const CubeSky*>(source);
            view.SkyCube = cube->Cube.get();
            view.EnvironmentIntensity = sky->Intensity;
            // A baked cube lights through the same radiance cube the skybox samples, so Intensity
            // scales its ambient exactly as a baked material sky's does.
            view.SkylightIntensity = sky->Intensity;
            resolved.Kind = SkySourceKind::Cube;
            view.SkyRotation = orientation;
            // A CubeSky is always a baked cube (a caller-owned radiance cube); it is never a direct
            // per-pixel source, so it always drives the skybox path and can light the scene.
            resolved.Baked = true;
        }
        return resolved;
    }
}
