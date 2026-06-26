#pragma once

#include <Veng/Veng.h>

namespace Veng
{
    class SystemRegistry;

    /// @brief Pre-registers the engine's builtin reusable SceneSystems into a catalog.
    ///
    /// The system analogue of RegisterBuiltinTypes: the host (launcher, editor, or cooker)
    /// calls this on its SystemRegistry before running a module's VengModuleRegister, so the
    /// engine's reusable systems (MovementSystem, CameraRigSystem, RootMotionDriveSystem,
    /// AnimationSystem, ConstantMotionSystem) are in the catalog a level can name without the
    /// game re-declaring them. A game registers only its **own** systems. Registration is
    /// catalog membership only — run order is the level's ordered SystemId set, not registration
    /// order. GPU-free: building a system touches no Context or device, so a headless cooker with
    /// no Vulkan ICD can call it.
    /// @param registry  The catalog to populate.
    VE_API void RegisterBuiltinSystems(SystemRegistry& registry);
}
