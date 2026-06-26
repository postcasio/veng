#include <Veng/Scene/BuiltinSystems.h>

#include <Veng/Scene/AnimationSystem.h>
#include <Veng/Scene/CameraRig.h>
#include <Veng/Scene/Motion.h>
#include <Veng/Scene/Movement.h>
#include <Veng/Scene/RootMotion.h>
#include <Veng/Scene/SystemRegistry.h>

namespace Veng
{
    void RegisterBuiltinSystems(SystemRegistry& registry)
    {
        // Builtins go through the same Register<T> path as game systems; no special-casing.
        // A level names the subset and order it runs — this only makes them resolvable.
        registry.Register<MovementSystem>();
        registry.Register<RootMotionDriveSystem>();
        registry.Register<CameraRigSystem>();
        registry.Register<AnimationSystem>();
        registry.Register<ConstantMotionSystem>();
    }
}
