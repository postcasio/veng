#include <Veng/Platform/FileBrowser.h>

#include <filesystem>
#include <system_error>

#include <fmt/format.h>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#else
#include <cerrno>
#include <cstring>
#include <spawn.h>
#include <sys/wait.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#else
#include <unistd.h>
extern char** environ;
#endif
#endif

namespace Veng
{
#if !defined(_WIN32)
    namespace
    {
        /// @brief Runs @p argv to completion, reporting a failure to start or a non-zero exit.
        VoidResult SpawnAndWait(const vector<const char*>& argv)
        {
#if defined(__APPLE__)
            char** const environment = *_NSGetEnviron();
#else
            char** const environment = environ;
#endif
            pid_t child = 0;
            const int spawned = posix_spawnp(&child, argv.front(), nullptr, nullptr,
                                             const_cast<char* const*>(argv.data()), environment);
            if (spawned != 0)
            {
                return std::unexpected(
                    fmt::format("could not run '{}': {}", argv.front(), std::strerror(spawned)));
            }

            int status = 0;
            while (waitpid(child, &status, 0) == -1)
            {
                if (errno != EINTR)
                {
                    return std::unexpected(fmt::format("could not wait for '{}': {}", argv.front(),
                                                       std::strerror(errno)));
                }
            }
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            {
                return std::unexpected(fmt::format("'{}' failed", argv.front()));
            }
            return {};
        }
    }
#endif

    VoidResult OpenInFileBrowser(const path& directory)
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(directory, ec))
        {
            return std::unexpected(fmt::format("'{}' is not a directory", directory.string()));
        }

#if defined(_WIN32)
        const HINSTANCE result = ShellExecuteW(nullptr, L"open", directory.wstring().c_str(),
                                               nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32)
        {
            return std::unexpected(
                fmt::format("the shell could not open '{}'", directory.string()));
        }
        return {};
#elif defined(__APPLE__)
        // `open` hands the directory to Finder through Launch Services and exits at once.
        const string target = directory.string();
        return SpawnAndWait({"open", target.c_str(), nullptr});
#else
        // xdg-open may stay running as the file manager itself, so it is backgrounded by a shell
        // that exits at once; the path rides as "$1", never through the shell's parsing.
        const string target = directory.string();
        return SpawnAndWait(
            {"sh", "-c", "xdg-open \"$1\" >/dev/null 2>&1 &", "sh", target.c_str(), nullptr});
#endif
    }
}
