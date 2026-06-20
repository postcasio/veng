#pragma once

#include <Veng/Veng.h>
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
        /// @param modulePath  Path to the shared library to load.
        /// @return The loaded module on success, or an error string on failure.
        [[nodiscard]] static Result<LoadedModule> Load(const path& modulePath);
    };
}
