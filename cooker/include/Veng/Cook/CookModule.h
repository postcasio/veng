#pragma once

#include <Veng/Veng.h>
#include <Veng/Assert.h>
#include <Veng/Result.h>
#include <Veng/Cook/Importer.h>
#include <Veng/Module/ModuleLoader.h>

namespace Veng::Cook
{
    class Cooker;

    /// @brief Host-owned catalog of importers a cook module contributes, keyed by asset type.
    ///
    /// The cook module's entire registration surface. It deliberately carries no
    /// AssetTypeRegistry: an asset type's identity and name register exactly once per process,
    /// through the runtime module's VengModuleRegister, which every host reaches. Registering
    /// from both entries would deliver the same id twice in the editor — where the runtime module
    /// and the cook module both load — and a duplicate id is fatal by design.
    class AssetImporterRegistry
    {
    public:
        /// @brief Records an importer under its declared Type(), aborting on a duplicate.
        ///
        /// Defined inline because this is the module's half of the contract: a cook module links
        /// veng::cook_interface, never libveng_cook, so an out-of-line definition would be an
        /// unresolved symbol in the dlopened image.
        /// @param importer  The importer to record; must be non-null.
        void Register(Unique<AssetImporter> importer)
        {
            VE_ASSERT(importer != nullptr, "AssetImporterRegistry: importer is null");

            const AssetTypeId type = importer->Type();
            VE_ASSERT(type.IsValid(),
                      "AssetImporterRegistry: importer claims the invalid asset type");
            for (const Unique<AssetImporter>& existing : m_Importers)
            {
                VE_ASSERT(existing->Type() != type,
                          "AssetImporterRegistry: asset type {:#018X} already has an importer",
                          type.Value);
            }

            m_Importers.push_back(std::move(importer));
        }

        /// @brief Returns the recorded importers, in registration order.
        [[nodiscard]] const vector<Unique<AssetImporter>>& All() const { return m_Importers; }

        /// @brief Moves every recorded importer into a cooker, leaving this registry empty.
        ///
        /// Host-side only, so it stays out of line — a cook module never calls it and so never
        /// needs the Cooker it names.
        /// @warning Ownership of code living in the module image moves with them: the cooker must
        ///          then be destroyed before the LoadedCookModule whose handle backs it.
        /// @param cooker  The cooker the importers are registered into.
        void MoveInto(Cooker& cooker);

    private:
        /// @brief Importers in registration order; the asset type each claims is its own Type().
        vector<Unique<AssetImporter>> m_Importers;
    };

    /// @brief The host side of the cook-module contract: the registries a cook module writes into.
    struct VengCookModuleHost
    {
        /// @brief Receives every AssetImporter the module defines.
        AssetImporterRegistry& Importers;
    };

    /// @brief A loaded cook module paired with the importers it registered.
    ///
    /// Declaration order is the lifetime contract, not a convention: Importers holds owned
    /// polymorphic objects whose vtables and code live in the dlclose-able image, so the handle
    /// is declared first and therefore destroyed last. The same ordering binds anything the
    /// importers are moved into.
    struct LoadedCookModule
    {
        /// @brief RAII dlopen handle; must outlive Importers and any cooker they move into.
        LoadedModule Module;
        /// @brief Every importer the module's VengCookModuleRegister contributed.
        AssetImporterRegistry Importers;
    };

    /// @brief Loads a cook module, runs its ABI handshake, and collects its importers.
    ///
    /// A stale or foreign library — one exporting no VengCookModuleAbiVersion, or one built
    /// against a different cook ABI — is a located Result error and its entry never runs.
    /// @param modulePath  Path to the cook-module shared library.
    /// @return The handle plus its registered importers, or a located error.
    [[nodiscard]] Result<LoadedCookModule> LoadCookModule(const path& modulePath);

    /// @brief Derives the conventional cook-module path sitting beside a runtime module.
    ///
    /// `<dir>/libgame.dylib` yields `<dir>/libgame_cook.dylib`: the same directory, the same
    /// extension, the stem suffixed. This is the lookup a host performs when no explicit cook
    /// module was named; the caller decides whether a missing file is an error.
    /// @param runtimeModulePath  Path to the runtime module the cook module sits beside.
    /// @return The sibling cook-module path.
    [[nodiscard]] path SiblingCookModulePath(const path& runtimeModulePath);

    /// @brief Inverts SiblingCookModulePath: the runtime module a cook module sits beside.
    ///
    /// `<dir>/libgame_cook.dylib` yields `<dir>/libgame.dylib`. A cook module links its runtime
    /// module, so that image is present whenever the cook module is — which is what lets a host
    /// given only the cook module still load the asset-type identities its importers are keyed on.
    /// @param cookModulePath  Path to a cook module.
    /// @return The sibling runtime-module path, or nullopt when the stem is not `_cook`-suffixed.
    [[nodiscard]] optional<path> SiblingRuntimeModulePath(const path& cookModulePath);
}

extern "C"
{
    /// @brief Entry point exported by every cook module.
    ///
    /// The host dlsym()s this name after the cook-ABI handshake passes, calls it once, and the
    /// module registers its importers into the provided host registry.
    VE_MODULE_EXPORT void VengCookModuleRegister(Veng::Cook::VengCookModuleHost* host);
}

/// @brief Cook-module ABI version token baked into both host and module at compile time.
///
/// Independent of VENG_MODULE_ABI_VERSION: the two contracts version separately, so a change to
/// the importer surface never invalidates every runtime module. Guarded with #ifndef so a target
/// can force a mismatch via -D for testing.
#ifndef VENG_COOK_MODULE_ABI_VERSION
#define VENG_COOK_MODULE_ABI_VERSION 1u
#endif

/// @brief Emits the VengCookModuleAbiVersion() export; place in exactly one TU per cook module.
#define VE_EXPORT_COOK_MODULE_ABI()                                                                \
    extern "C" VE_MODULE_EXPORT Veng::u32 VengCookModuleAbiVersion()                               \
    {                                                                                              \
        return VENG_COOK_MODULE_ABI_VERSION;                                                       \
    }
