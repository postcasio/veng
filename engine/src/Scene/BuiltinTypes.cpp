#include <Veng/Scene/BuiltinTypes.h>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Camera.h>

namespace Veng
{
    void RegisterBuiltinTypes(TypeRegistry& registry)
    {
        // The builtin components, through the same Register<T> path a game uses
        // for its own types — builtins are not special-cased. Each Register<T>()
        // is idempotent and auto-registers its field types' leaf vocabulary, so
        // the order here is robustness, not a requirement.
        registry.Register<Name>();
        registry.Register<Transform>();
        registry.Register<Parent>();
        registry.Register<CameraComponent>();
        registry.Register<MeshRenderer>();
        registry.Register<LightType>();
        registry.Register<Light>();
    }
}
