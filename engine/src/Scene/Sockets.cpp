#include <Veng/Scene/Sockets.h>

#include <Veng/Asset/Mesh.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

namespace Veng
{
    const MeshSocket* FindMeshSocket(const Scene& scene, const Entity meshEntity,
                                     const std::string_view socketName)
    {
        if (meshEntity.IsNull() || !scene.IsAlive(meshEntity))
        {
            return nullptr;
        }

        const auto* renderer = scene.TryGet<MeshRenderer>(meshEntity);
        if (renderer == nullptr || !renderer->Mesh.IsLoaded())
        {
            return nullptr;
        }

        return renderer->Mesh.Get()->FindSocket(socketName);
    }

    bool AttachToSocket(Scene& scene, const Entity child, const Entity meshEntity,
                        const std::string_view socketName)
    {
        const MeshSocket* socket = FindMeshSocket(scene, meshEntity, socketName);
        if (socket == nullptr)
        {
            return false;
        }

        scene.SetParent(child, meshEntity);

        Transform& transform =
            scene.Has<Transform>(child) ? scene.Get<Transform>(child) : scene.Add<Transform>(child);
        transform.Position = socket->Position;
        transform.Rotation = socket->Rotation;
        transform.Scale = socket->Scale;
        return true;
    }
}
