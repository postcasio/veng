#pragma once

#include <span>

#include <Veng/Asset/Types.h>
#include <Veng/Asset/Path.h>

namespace Veng
{
    /// @brief Writes bytes to a file atomically: a uniquely-named temporary in the
    /// destination directory is written in full, then renamed over the final path.
    ///
    /// The final path never holds partial content — a concurrent reader (or an
    /// mtime-driven build step) sees either the previous complete file or the new
    /// complete one, never a torn write. An interrupted or killed writer leaves the
    /// destination untouched with its old timestamp, so a build step whose output
    /// this is re-runs instead of treating a truncated artifact as up to date.
    /// @param filePath  Destination path; atomically replaced if it already exists.
    /// @param bytes     The complete file content.
    /// @return An error string on failure; the destination is left unchanged.
    [[nodiscard]] VoidResult WriteFileAtomic(const path& filePath, std::span<const u8> bytes);
}
