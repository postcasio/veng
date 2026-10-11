#include <Veng/Capture/UserMedia.h>

#include <Veng/Platform/UserPaths.h>

#include <filesystem>
#include <system_error>

#include <fmt/format.h>

namespace Veng::Capture
{
    namespace
    {
        /// @brief Resolves `UserDataDir(application) / folder`, creating it when absent.
        Result<path> EnsureMediaFolder(const string_view application, const string_view folder)
        {
            const Result<path> base = UserDataDir(application);
            if (!base.has_value())
            {
                return std::unexpected(base.error());
            }

            const path dir = *base / string(folder);
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if (ec)
            {
                return std::unexpected(
                    fmt::format("could not create directory '{}': {}", dir.string(), ec.message()));
            }
            return dir;
        }
    }

    Result<path> VideoDirectory(const string_view application)
    {
        return EnsureMediaFolder(application, VideosFolder);
    }
}
