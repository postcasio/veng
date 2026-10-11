#pragma once

#include <Veng/Veng.h>
#include <Veng/Path.h>
#include <Veng/Result.h>

/// @brief Where the media an application captures for its user lands on disk.
///
/// A recording or a screenshot is the user's own file, not a build artefact, so it lives under the
/// per-user data directory (UserDataDir) keyed by the application's name, in one folder per kind.
/// Nothing here needs the host to configure a path: the application's name is the whole key.
namespace Veng::Capture
{
    /// @brief The folder under UserDataDir(application) that video recordings land in.
    inline constexpr string_view VideosFolder = "videos";

    /// @brief The folder under UserDataDir(application) that screenshots land in.
    inline constexpr string_view ScreenshotsFolder = "screenshots";

    /// @brief Resolves, and creates if absent, the directory an application's recordings land in.
    ///
    /// `UserDataDir(application) / VideosFolder` — a locate-and-ensure call, like UserDataDir.
    /// @param application  The application's name: the single segment its user directories share.
    /// @return The ready, existing directory, or an error saying why it could not be resolved or
    ///         created.
    [[nodiscard]] VE_API Result<path> VideoDirectory(string_view application);

    /// @brief Resolves, and creates if absent, the directory an application's screenshots land in.
    ///
    /// `UserDataDir(application) / ScreenshotsFolder` — a locate-and-ensure call, like UserDataDir.
    /// @param application  The application's name: the single segment its user directories share.
    /// @return The ready, existing directory, or an error saying why it could not be resolved or
    ///         created.
    [[nodiscard]] VE_API Result<path> ScreenshotDirectory(string_view application);
}
