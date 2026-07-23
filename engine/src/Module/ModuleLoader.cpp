#include <Veng/Module/ModuleLoader.h>
#include <Veng/Path.h>

#include <Veng/Assert.h>

#include <fmt/format.h>

#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace Veng
{
    namespace
    {
#if defined(_WIN32)
        string LastLoaderError()
        {
            const DWORD code = GetLastError();
            if (code == 0)
            {
                return "unknown error";
            }

            LPSTR buffer = nullptr;
            const DWORD length =
                FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                   FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                               reinterpret_cast<LPSTR>(&buffer), 0, nullptr);

            string message = (buffer && length) ? string(buffer, length) : "unknown error";
            if (buffer)
            {
                LocalFree(buffer);
            }
            return message;
        }
#endif
    }

    LoadedModule::~LoadedModule()
    {
        if (m_Handle)
        {
#if defined(_WIN32)
            FreeLibrary(static_cast<HMODULE>(m_Handle));
#else
            dlclose(m_Handle);
#endif
        }
    }

    LoadedModule::LoadedModule(LoadedModule&& other) noexcept
        : m_Handle(std::exchange(other.m_Handle, nullptr))
    {
    }

    LoadedModule& LoadedModule::operator=(LoadedModule&& other) noexcept
    {
        if (this != &other)
        {
            if (m_Handle)
            {
#if defined(_WIN32)
                FreeLibrary(static_cast<HMODULE>(m_Handle));
#else
                dlclose(m_Handle);
#endif
            }
            m_Handle = std::exchange(other.m_Handle, nullptr);
        }
        return *this;
    }

    void* LoadedModule::Resolve(const char* symbol) const
    {
#if defined(_WIN32)
        return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(m_Handle), symbol));
#else
        return dlsym(m_Handle, symbol);
#endif
    }

    void LoadedModule::Register(VengModuleHost& host) const
    {
        using EntryFn = void (*)(VengModuleHost*);

        auto entry = reinterpret_cast<EntryFn>(Resolve("VengModuleRegister"));

        VE_ASSERT(entry != nullptr,
                  "module is version-matched but exports no VengModuleRegister entry");
        entry(&host);
    }

    Result<LoadedModule> ModuleLoader::Load(const path& modulePath, const char* versionSymbol,
                                            const u32 expectedVersion)
    {
        using VersionFn = u32 (*)();

#if defined(_WIN32)
        HMODULE handle = LoadLibraryW(modulePath.c_str());
        if (!handle)
        {
            return std::unexpected(fmt::format("failed to load module '{}': {}",
                                               modulePath.string(), LastLoaderError()));
        }

        auto versionFn = reinterpret_cast<VersionFn>(GetProcAddress(handle, versionSymbol));
#else
        void* handle = dlopen(modulePath.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle)
        {
            const char* error = dlerror();
            return std::unexpected(fmt::format("failed to load module '{}': {}",
                                               modulePath.string(),
                                               error ? error : "unknown error"));
        }

        auto versionFn = reinterpret_cast<VersionFn>(dlsym(handle, versionSymbol));
#endif

        if (!versionFn)
        {
#if defined(_WIN32)
            FreeLibrary(handle);
#else
            dlclose(handle);
#endif
            return std::unexpected(
                fmt::format("module '{}' exports no {} — not a veng module of this kind "
                            "(host expects ABI v{})",
                            modulePath.string(), versionSymbol, expectedVersion));
        }

        const u32 moduleVersion = versionFn();
        if (moduleVersion != expectedVersion)
        {
#if defined(_WIN32)
            FreeLibrary(handle);
#else
            dlclose(handle);
#endif
            return std::unexpected(
                fmt::format("module '{}' built against ABI v{}, host expects v{}",
                            modulePath.string(), moduleVersion, expectedVersion));
        }

        LoadedModule module;
        module.m_Handle = handle;
        return module;
    }
}
