#include <Veng/Scene/Camera.h>

#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

namespace Veng
{
    namespace
    {
        // Projects a camera entity through its world matrix, or nullopt if it lacks
        // either component. Shared by both resolve entry points.
        optional<CameraView> ResolveCameraEntity(const Scene& scene, Entity camera, f32 aspect)
        {
            if (camera == Entity::Null || !scene.IsAlive(camera))
            {
                return std::nullopt;
            }

            const auto* component = scene.TryGet<Camera>(camera);
            if (component == nullptr || !scene.Has<Transform>(camera))
            {
                return std::nullopt;
            }

            return MakeCameraView(*component, aspect, WorldMatrix(scene, camera));
        }
    }

    optional<CameraView> ResolveCameraView(const Scene& scene, const Entity viewer,
                                           const f32 aspect)
    {
        if (viewer == Entity::Null || !scene.IsAlive(viewer))
        {
            return std::nullopt;
        }

        const auto* seat = scene.TryGet<Viewer>(viewer);
        if (seat == nullptr)
        {
            return std::nullopt;
        }

        return ResolveCameraEntity(scene, seat->Camera, aspect);
    }

    optional<CameraView> ResolvePrimaryCameraView(const Scene& scene, const f32 aspect)
    {
        for (auto [seatEntity, seat] : scene.View<const Viewer>())
        {
            if (optional<CameraView> view = ResolveCameraEntity(scene, seat.Camera, aspect))
            {
                return view;
            }
        }

        // No Viewer resolved a camera; fall back to the first bare (Transform, Camera)
        // entity so a one-camera scene with no authored seat still renders.
        for (auto [cameraEntity, transform, camera] : scene.View<const Transform, const Camera>())
        {
            return MakeCameraView(camera, aspect, WorldMatrix(scene, cameraEntity));
        }

        return std::nullopt;
    }
}
