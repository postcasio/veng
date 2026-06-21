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

        // Registering Primitive transitively registers its shape variant and the shape
        // alternatives through the dependency recursion.
        registry.Register<Primitive>();
    }
}
