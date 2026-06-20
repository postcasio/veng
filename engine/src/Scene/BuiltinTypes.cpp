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
        registry.Register<Parent>();
        registry.Register<CameraComponent>();
        registry.Register<MeshRenderer>();
        registry.Register<LightType>();
        registry.Register<Light>();
    }
}
