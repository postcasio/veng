#pragma once

#include <Veng/Result.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Module/ModuleLoader.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/SystemRegistry.h>

namespace Veng::Cook
{
    /// @brief The registries a loaded game module filled, paired with the image they point into.
    ///
    /// Component strings, lifecycle thunks, system factories, and asset-type names live in the
    /// loaded module, so the handle must outlive every use of any registry. Move-only
    /// (LoadedModule is non-copyable). Declaration order ensures the registries are destroyed
    /// before Module, so dlclose runs last.
    struct LoadedModuleTypes
    {
        /// @brief RAII dlopen handle; must outlive every registry below.
        LoadedModule Module;
        /// @brief Engine builtins plus every type the module registered.
        TypeRegistry Types;
        /// @brief Every SceneSystem the module registered; the catalog the level importer resolves ids against.
        SystemRegistry Systems;
        /// @brief Engine builtin asset types plus every asset type the module registered.
        ///
        /// What lets a pack manifest name a module-defined type by its registered name, and what
        /// `generate-asset-type --module` mints against.
        AssetTypeRegistry AssetTypes;
    };

    /// @brief Copies a module's asset-type registrations into another registry, skipping known ids.
    ///
    /// The merge a host performs after LoadModuleTypes to teach its own registry — a Cooker's, an
    /// editor's — the module's type names. Ids the destination already carries (the engine
    /// builtins, present in both) are skipped rather than re-registered, which would abort.
    /// @param source       The loaded module's registry.
    /// @param destination  The host registry receiving the module's own types.
    void MergeAssetTypes(const AssetTypeRegistry& source, AssetTypeRegistry& destination);

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
