#pragma once

#include <Veng/Cook/Types.h>

#include <string_view>

namespace Veng::Cook
{
    /// @brief Reads and parses a JSON file into an object document.
    ///
    /// The shared read → parse → validate preamble every JSON source load runs.
    /// On failure returns a located message — "<what> '<file>': failed to open" or
    /// "<what> '<file>': invalid JSON" — matching the importers' located-error
    /// discipline. A non-object root is rejected as invalid JSON.
    /// @param file  The JSON file to read.
    /// @param what  The diagnostic noun locating the caller (e.g. "texture importer", "pack").
    /// @return The parsed object document, or the located error.
    [[nodiscard]] Result<json> ReadJsonFile(const path& file, std::string_view what);
}
