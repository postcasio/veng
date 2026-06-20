#pragma once

#include <Veng/Result.h>
#include <Veng/Module/ModuleLoader.h>
#include <Veng/Reflection/TypeRegistry.h>

namespace Veng::Cook
{
    /// @brief A populated TypeRegistry paired with the module image whose descriptors it points into.
    ///
    /// Component strings and lifecycle thunks live in the loaded module, so the handle must outlive
    /// every use of the registry. Move-only (LoadedModule is non-copyable). Declaration order ensures
    /// Types is destroyed before Module, so dlclose runs last.
    struct LoadedModuleTypes
    {
        /// @brief RAII dlopen handle; must outlive Types.
        LoadedModule Module;
        /// @brief Engine builtins plus every type the module registered.
        TypeRegistry Types;
    };

    /// @brief Loads a game module and returns its reflected type registry paired with the live module handle.
    ///
    /// Engine builtins are pre-registered via RegisterBuiltinTypes, then the module's VengModuleRegister
    /// adds the game's component types. The Application factory the module also registers is captured
    /// into a throwaway ApplicationRegistry and never invoked — the cooker constructs no app.
    /// GPU-free: no Context or Vulkan device is created.
    /// @param modulePath  Path to the game module shared library.
    /// @return Populated LoadedModuleTypes, or a located error on load or ABI failure.
    [[nodiscard]] Result<LoadedModuleTypes> LoadModuleTypes(const path& modulePath);

    /// @brief Mints a fresh non-zero TypeId that collides with no id in the given registry.
    ///
    /// The TypeId analogue of GenerateAssetId. Pass a registry holding the engine builtins
    /// (and the game's types when using `--module`) so the result is unique across all known ids.
    /// @param existing  Registry to check for collisions.
    /// @return A fresh, collision-free TypeId.
    [[nodiscard]] TypeId GenerateTypeId(const TypeRegistry& existing);
}
