#pragma once

#include <Veng/Veng.h>
#include <Veng/Result.h>

namespace Veng
{
    /// @brief The per-OS writable base directory for an application's persistent data.
    ///
    /// Resolves the platform's writable application-data base, appends an
    /// @p application segment, and creates the directory (and any missing parents)
    /// if absent — a locate-and-ensure call, not a pure lookup. macOS: `~/Library/
    /// Application Support/<application>`; Windows: `%APPDATA%/<application>`;
    /// Linux: `$XDG_DATA_HOME/<application>` or `~/.local/share/<application>` when
    /// unset. A base that cannot be resolved (no `HOME`/`%APPDATA%`) or a directory
    /// that cannot be created is a recoverable Result error, never a crash.
    /// @param application  The single application segment appended to the platform base.
    /// @return The ready, existing directory, or an error describing why it could not
    /// be resolved or created.
    [[nodiscard]] VE_API Result<path> UserDataDir(string_view application);

    /// @brief The per-OS writable base directory for an application's user configuration.
    ///
    /// Same locate-and-ensure contract as UserDataDir(). Linux additionally honors
    /// `$XDG_CONFIG_HOME` (falling back to `~/.config/<application>`); macOS and
    /// Windows resolve to the same base as UserDataDir(), since neither platform
    /// distinguishes a separate config location.
    /// @param application  The single application segment appended to the platform base.
    /// @return The ready, existing directory, or an error describing why it could not
    /// be resolved or created.
    [[nodiscard]] VE_API Result<path> UserConfigDir(string_view application);

    /// @brief The per-OS writable base directory for an application's expendable caches.
    ///
    /// Same locate-and-ensure contract as UserDataDir(). Linux additionally honors
    /// `$XDG_CACHE_HOME` (falling back to `~/.cache/<application>`); macOS and Windows
    /// resolve to the same base as UserDataDir(). Content under this directory is
    /// expendable — a caller may not assume it survives across runs.
    /// @param application  The single application segment appended to the platform base.
    /// @return The ready, existing directory, or an error describing why it could not
    /// be resolved or created.
    [[nodiscard]] VE_API Result<path> UserCacheDir(string_view application);
}
