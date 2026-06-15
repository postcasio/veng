#pragma once

#include <Veng/Veng.h>
#include <Veng/Result.h>
#include <Veng/Module/Module.h>

namespace Veng
{
    // RAII handle over a loaded shared library. Closes the library on
    // destruction. Returned by value through Result<LoadedModule>, so it must
    // move correctly: a missed handle null-out is a double-close, fatal under
    // -fno-exceptions.
    class VE_API LoadedModule
    {
    public:
        ~LoadedModule();                       // dlclose / FreeLibrary
        LoadedModule(LoadedModule&&) noexcept;
        LoadedModule& operator=(LoadedModule&&) noexcept;

        LoadedModule(const LoadedModule&) = delete;
        LoadedModule& operator=(const LoadedModule&) = delete;

        // Resolve the module's entry and call it once with the host. Fatal if the
        // entry is missing — a version-matched module without VengModuleRegister
        // is a build error surfaced at load, not a recoverable condition.
        void Register(VengModuleHost& host) const;

    private:
        friend class ModuleLoader;
        LoadedModule() = default;

        void* m_Handle = nullptr; // dlopen / LoadLibrary handle (opaque)
    };

    class VE_API ModuleLoader
    {
    public:
        // Load a shared library by path and verify its ABI version. Recoverable:
        // a missing/unloadable file, a missing version symbol, or a version
        // mismatch is a Result error (the launcher reports it and exits), not an
        // assert.
        [[nodiscard]] static Result<LoadedModule> Load(const path& modulePath);
    };
}
