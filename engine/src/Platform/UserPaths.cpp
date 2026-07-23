#include <Veng/Platform/UserPaths.h>
#include <Veng/Path.h>

#include <cstdlib>
#include <system_error>

#include <fmt/format.h>

#if defined(_WIN32)
#include <windows.h>
#include <shlobj.h>
#endif

namespace Veng
{
    namespace
    {
        /// @brief Reads an environment variable, treating an empty value as unset.
        ///
        /// XDG's own spec says an empty XDG_* variable is to be treated as if unset,
        /// so callers get a consistent "absent" signal from either case.
        optional<string> GetEnv(const char* name)
        {
            const char* value = std::getenv(name);
            if (value == nullptr || value[0] == '\0')
            {
                return std::nullopt;
            }
            return string(value);
        }

        /// @brief Creates @p dir (and any missing parents), reporting a Result error on failure.
        Result<path> EnsureDirectory(path dir)
        {
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if (ec)
            {
                return std::unexpected(
                    fmt::format("could not create directory '{}': {}", dir.string(), ec.message()));
            }
            return dir;
        }

#if defined(_WIN32)
        /// @brief Resolves %APPDATA%, the shared Windows base for data/config/cache.
        Result<path> WindowsAppDataBase()
        {
            const optional<string> appData = GetEnv("APPDATA");
            if (!appData.has_value())
            {
                return std::unexpected(
                    string("could not resolve %APPDATA%: environment variable unset"));
            }
            return path(*appData);
        }
#elif defined(__APPLE__)
        /// @brief Resolves ~/Library/Application Support, the shared macOS base for data/config/cache.
        Result<path> MacApplicationSupportBase()
        {
            const optional<string> home = GetEnv("HOME");
            if (!home.has_value())
            {
                return std::unexpected(
                    string("could not resolve HOME: environment variable unset"));
            }
            return path(*home) / "Library" / "Application Support";
        }
#else
        /// @brief Resolves the Linux XDG base for @p xdgVar, falling back to ~/<fallback>.
        Result<path> LinuxXdgBase(const char* xdgVar, string_view fallback)
        {
            const optional<string> xdg = GetEnv(xdgVar);
            if (xdg.has_value())
            {
                return path(*xdg);
            }

            const optional<string> home = GetEnv("HOME");
            if (!home.has_value())
            {
                return std::unexpected(fmt::format(
                    "could not resolve {} or HOME: both environment variables unset", xdgVar));
            }
            return path(*home) / fallback;
        }
#endif
    }

    Result<path> UserDataDir(string_view application)
    {
#if defined(_WIN32)
        const Result<path> base = WindowsAppDataBase();
#elif defined(__APPLE__)
        const Result<path> base = MacApplicationSupportBase();
#else
        const Result<path> base = LinuxXdgBase("XDG_DATA_HOME", ".local/share");
#endif
        if (!base.has_value())
        {
            return std::unexpected(base.error());
        }
        return EnsureDirectory(*base / string(application));
    }

    Result<path> UserConfigDir(string_view application)
    {
#if defined(_WIN32)
        const Result<path> base = WindowsAppDataBase();
#elif defined(__APPLE__)
        const Result<path> base = MacApplicationSupportBase();
#else
        const Result<path> base = LinuxXdgBase("XDG_CONFIG_HOME", ".config");
#endif
        if (!base.has_value())
        {
            return std::unexpected(base.error());
        }
        return EnsureDirectory(*base / string(application));
    }

    Result<path> UserCacheDir(string_view application)
    {
#if defined(_WIN32)
        const Result<path> base = WindowsAppDataBase();
#elif defined(__APPLE__)
        const Result<path> base = MacApplicationSupportBase();
#else
        const Result<path> base = LinuxXdgBase("XDG_CACHE_HOME", ".cache");
#endif
        if (!base.has_value())
        {
            return std::unexpected(base.error());
        }
        return EnsureDirectory(*base / string(application));
    }
}
