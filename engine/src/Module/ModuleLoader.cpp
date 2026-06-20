#include <Veng/Module/ModuleLoader.h>

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

    void LoadedModule::Register(VengModuleHost& host) const
    {
        using EntryFn = void (*)(VengModuleHost*);

#if defined(_WIN32)
        auto entry = reinterpret_cast<EntryFn>(
            GetProcAddress(static_cast<HMODULE>(m_Handle), "VengModuleRegister"));
#else
        auto entry = reinterpret_cast<EntryFn>(dlsym(m_Handle, "VengModuleRegister"));
#endif

        VE_ASSERT(entry != nullptr,
                  "module is version-matched but exports no VengModuleRegister entry");
        entry(&host);
    }

    Result<LoadedModule> ModuleLoader::Load(const path& modulePath)
    {
        using VersionFn = u32 (*)();

#if defined(_WIN32)
        HMODULE handle = LoadLibraryW(modulePath.c_str());
        if (!handle)
        {
            return std::unexpected(fmt::format("failed to load module '{}': {}",
                                               modulePath.string(), LastLoaderError()));
        }

        auto versionSymbol =
            reinterpret_cast<VersionFn>(GetProcAddress(handle, "VengModuleAbiVersion"));
#else
        void* handle = dlopen(modulePath.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle)
        {
            const char* error = dlerror();
            return std::unexpected(fmt::format("failed to load module '{}': {}",
                                               modulePath.string(),
                                               error ? error : "unknown error"));
        }

        auto versionSymbol = reinterpret_cast<VersionFn>(dlsym(handle, "VengModuleAbiVersion"));
#endif

        if (!versionSymbol)
        {
#if defined(_WIN32)
            FreeLibrary(handle);
#else
            dlclose(handle);
#endif
            return std::unexpected(
                fmt::format("module '{}' exports no VengModuleAbiVersion — not a veng module "
                            "(engine expects ABI v{})",
                            modulePath.string(), VENG_MODULE_ABI_VERSION));
        }

        const u32 moduleVersion = versionSymbol();
        if (moduleVersion != VENG_MODULE_ABI_VERSION)
        {
#if defined(_WIN32)
            FreeLibrary(handle);
#else
            dlclose(handle);
#endif
            return std::unexpected(
                fmt::format("module '{}' built against ABI v{}, engine expects v{}",
                            modulePath.string(), moduleVersion, VENG_MODULE_ABI_VERSION));
        }

        LoadedModule module;
        module.m_Handle = handle;
        return module;
    }
}
