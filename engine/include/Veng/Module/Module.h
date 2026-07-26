#pragma once

#include <Veng/Veng.h>
#include <Veng/Module/ApplicationRegistry.h>
#include <Veng/Reflection/TypeRegistry.h>

namespace Veng
{
    /// @brief Forward-declared editor registry; libveng never sees its definition.
    ///
    /// A non-editor host passes Editor = nullptr. Held by pointer so the incomplete type suffices.
    class EditorRegistry;

    /// @brief Forward-declared so the reference member needs no include here.
    class SystemRegistry;

    /// @brief Forward-declared so the pointer member needs no include here.
    class GuiDriverRegistry;

    /// @brief Forward-declared so the reference member needs no include here.
    class AssetTypeRegistry;

    /// @brief Forward-declared so the reference member needs no include here.
    class AssetLoaderRegistry;

    /// @brief The host-side module contract: the registries a loaded module writes into.
    ///
    /// The host owns these for the module's whole lifetime. Registration is GPU-free
    /// (a factory + reflected type descriptors + asset-type identities and loader factories),
    /// so no live Context/AssetManager is required; the host threads them into the Application
    /// it later constructs.
    struct VengModuleHost
    {
        /// @brief Receives the module's Application factory.
        ApplicationRegistry& App;
        /// @brief Receives the module's component/type descriptors.
        TypeRegistry& Types;
        /// @brief Receives the module's SceneSystem registrations, in run order.
        SystemRegistry& Systems;
        /// @brief Receives the module's asset-type identities, names, and display metadata.
        ///
        /// The sole seam an asset type registers its identity through, reachable from every host
        /// (launcher, cooker, editor). A cook module registers importers only, so one id can never
        /// arrive twice in the editor, where both images load — and a duplicate id is fatal.
        AssetTypeRegistry& AssetTypes;
        /// @brief Receives the module's AssetLoader factories, one per asset type it defines.
        ///
        /// Instantiated by the AssetManager at construction. A host with no live AssetManager (the
        /// cooker) passes a throwaway whose registrations are inert and discarded.
        AssetLoaderRegistry& AssetLoaders;
        /// @brief Receives the module's GuiDriver registrations (per-instance UI presentation drivers).
        GuiDriverRegistry* Drivers;
        /// @brief Non-null only when loaded by the editor host.
        EditorRegistry* Editor;
    };
}

extern "C"
{
    /// @brief Entry point exported by every game/editor module.
    ///
    /// The host dlsym()s this name, calls it once after load, and the module
    /// registers its factory and types into the provided host registries.
    /// C ABI ensures the symbol resolves robustly across module boundaries.
    VE_MODULE_EXPORT void VengModuleRegister(Veng::VengModuleHost* host);
}

/// @brief ABI version token baked into both host and module at compile time.
///
/// Bumped whenever VengModuleHost's layout, or the layout of anything a module passes through
/// it, changes — a stale module registering a FieldDescriptor or AssetTypeInfo of the wrong size
/// is exactly the silent corruption this token exists to turn into a loud rejection.
/// The loader compares host vs. module values before calling VengModuleRegister.
/// Guarded with #ifndef so a target can force a mismatch via -D for testing.
#ifndef VENG_MODULE_ABI_VERSION
#define VENG_MODULE_ABI_VERSION 9u
#endif

/// @brief Emits the VengModuleAbiVersion() export; place in exactly one TU per module.
#define VE_EXPORT_MODULE_ABI()                                                                     \
    extern "C" VE_MODULE_EXPORT Veng::u32 VengModuleAbiVersion()                                   \
    {                                                                                              \
        return VENG_MODULE_ABI_VERSION;                                                            \
    }
