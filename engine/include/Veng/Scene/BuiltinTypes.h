#pragma once

#include <Veng/Veng.h>

namespace Veng
{
    class TypeRegistry;

    // Pre-registers the engine's builtin reflected types — the leaf vocabulary
    // and the builtin components (Name, Transform, Parent, CameraComponent,
    // MeshRenderer) — into a registry. GPU-free: touches no Context and no
    // device, so a headless cooker with no Vulkan ICD can call it. Idempotent
    // per type (re-registering an id is a no-op), so calling it twice is safe.
    VE_API void RegisterBuiltinTypes(TypeRegistry& registry);
}
