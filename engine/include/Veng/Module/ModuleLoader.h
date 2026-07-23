#pragma once

#include <Veng/Veng.h>
#include <Veng/Path.h>
#include <Veng/Result.h>
#include <Veng/Module/Module.h>

namespace Veng
{
    /// @brief RAII handle over a loaded shared library; closes the library on destruction.
    ///
    /// Returned by value through Result<LoadedModule>, so it must move correctly:
    /// a missed handle null-out in the move constructor would produce a double-close.
    class VE_API LoadedModule
    {
    public:
        /// @brief Closes the library (dlclose / FreeLibrary).
        ~LoadedModule();
        /// @brief Transfers ownership; nulls the source handle.
        LoadedModule(LoadedModule&&) noexcept;
        /// @brief Transfers ownership; nulls the source handle.
        LoadedModule& operator=(LoadedModule&&) noexcept;

        LoadedModule(const LoadedModule&) = delete;
        LoadedModule& operator=(const LoadedModule&) = delete;

        /// @brief Resolves VengModuleRegister and calls it once with the given host.
        ///
        /// Fatal if the entry is missing — a version-matched module without
        /// VengModuleRegister is a build error surfaced at load, not a recoverable condition.
        /// @param host  Host registries the module writes into.
        void Register(VengModuleHost& host) const;

        /// @brief Resolves an exported symbol by name, or null when the module exports none.
        ///
        /// The generic entry resolver behind Register: a host defining its own C-ABI entry
        /// (the cooker's importer-registration entry) resolves it through this rather than
        /// reimplementing the platform loader. The returned pointer is valid only while this
        /// handle is alive.
        /// @param symbol  The exported symbol name to resolve.
        /// @return The symbol's address, or nullptr when the module exports no such name.
        [[nodiscard]] void* Resolve(const char* symbol) const;

    private:
        friend class ModuleLoader;
        LoadedModule() = default;

        /// @brief dlopen / LoadLibrary handle (opaque).
        void* m_Handle = nullptr;
    };

    /// @brief Loads a shared library and verifies its ABI version.
    class VE_API ModuleLoader
    {
    public:
        /// @brief Loads a shared library by path and verifies its ABI version.
        ///
        /// A missing/unloadable file, a missing version symbol, or a version mismatch
        /// is returned as a Result error — the launcher reports it and exits.
        /// The version symbol and expected value are parameters so a second C-ABI contract
        /// carried by the same image (the cooker's importer entry) gets its own independent
        /// handshake without a second platform loader.
        /// @param modulePath      Path to the shared library to load.
        /// @param versionSymbol   Exported no-argument function returning the module's ABI version.
        /// @param expectedVersion The version the host was built against.
        /// @return The loaded module on success, or an error string on failure.
        [[nodiscard]] static Result<LoadedModule>
        Load(const path& modulePath, const char* versionSymbol = "VengModuleAbiVersion",
             u32 expectedVersion = VENG_MODULE_ABI_VERSION);
    };
}
