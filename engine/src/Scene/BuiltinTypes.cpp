#include <Veng/Scene/BuiltinTypes.h>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Camera.h>

namespace Veng
{
    void RegisterBuiltinTypes(TypeRegistry& registry)
    {
        // Builtins go through the same Register<T> path as game types; no special-casing.
        registry.Register<Name>();
        registry.Register<Transform>();
        registry.Register<Hierarchy>();
        registry.Register<Camera>();
        registry.Register<Viewer>();
        registry.Register<MeshRenderer>();
        registry.Register<LightType>();
        registry.Register<Light>();

        // The control pipeline: per-player snapshot, abstract command, seat→pawn link,
        // and per-pawn movement tuning.
        registry.Register<PlayerInput>();
        registry.Register<Intent>();
        registry.Register<Possesses>();
        registry.Register<Mover>();

        // Net-anticipation seam: the ownership annotation and the camera-rig follow
        // relationship the View-phase rig reads.
        registry.Register<Tier>();
        registry.Register<Authority>();
        registry.Register<CameraFollow>();

        // Registering Primitive transitively registers its shape variant and the shape
        // alternatives through the dependency recursion.
        registry.Register<Primitive>();
    }
}
