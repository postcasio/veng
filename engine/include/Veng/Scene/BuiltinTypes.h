#pragma once

#include <Veng/Veng.h>

namespace Veng
{
    class TypeRegistry;

    /// @brief Pre-registers the engine's builtin reflected types into a registry.
    ///
    /// Covers the leaf vocabulary and every engine-defined component, enum and
    /// cooked-data type a level or prefab may name. GPU-free: touches no
    /// Context or device, so a headless cooker with no Vulkan ICD can call it.
    /// Idempotent per type — re-registering an id is a no-op.
    /// @param registry  The registry to populate.
    VE_API void RegisterBuiltinTypes(TypeRegistry& registry);
}
