#pragma once

#include <Veng/Result.h>
#include <Veng/Module/ModuleLoader.h>
#include <Veng/Reflection/TypeRegistry.h>

// The cooker-side module reflection. Loading a game module into the cooker
// process pulls in libveng (the scoped dependency inversion: the prefab-cooking
// path links veng::veng to call ModuleLoader/TypeRegistry symbols). The graphics
// stack is linked but never initialized — no Context, no device — so this runs
// headless with no Vulkan ICD.

namespace Veng::Cook
{
    // A populated TypeRegistry paired with the module image whose descriptors it
    // points into. A component's strings and lifecycle thunks live in the loaded
    // module, so the handle must outlive every use of the registry. Move-only
    // (LoadedModule is non-copyable); declaration order destroys Types before
    // Module, so dlclose runs last.
    struct LoadedModuleTypes
    {
        LoadedModule Module; // the RAII dlopen handle (must outlive Types)
        TypeRegistry Types;  // builtins + every type the module registered
    };

    // Loads the game module, runs its VengModuleRegister against a cooker-owned
    // host carrying a fresh TypeRegistry (engine builtins pre-registered via
    // RegisterBuiltinTypes), and returns the populated registry paired with the
    // live module handle. The Application factory the module also registers is
    // captured into a throwaway ApplicationRegistry and never invoked (the cooker
    // constructs no app); Editor is null. GPU-free — no Context is created.
    // Located Result error on load / ABI failure.
    [[nodiscard]] Result<LoadedModuleTypes> LoadModuleTypes(const path& modulePath);

    // Mints a fresh non-zero TypeId that collides with no id registered in the
    // given registry — the TypeId analogue of GenerateAssetId. The caller passes
    // a registry holding the builtins (and, on --module, the game's types) so the
    // minted id is unique across everything already known.
    [[nodiscard]] TypeId GenerateTypeId(const TypeRegistry& existing);
}
