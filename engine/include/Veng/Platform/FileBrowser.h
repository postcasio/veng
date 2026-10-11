#pragma once

#include <Veng/Veng.h>
#include <Veng/Path.h>
#include <Veng/Result.h>

namespace Veng
{
    /// @brief Opens @p directory in the platform's file browser — Finder, Explorer, or whatever the
    /// desktop's `xdg-open` hands a directory to.
    ///
    /// Returns once the request is handed to the platform, without waiting for the window. The path
    /// travels as an argument, never through a shell's parsing, so any name opens as itself.
    /// @param directory  The directory to open; must exist.
    /// @return Success, or why the platform could not be asked.
    [[nodiscard]] VE_API VoidResult OpenInFileBrowser(const path& directory);
}
