#pragma once

#include <string>
#include <vector>

#include <slang/slang.h>

#include <Veng/Veng.h>

// Cooker-internal Slang session search-path policy. The three session setups
// (ShaderImporter's compile, SlangReflect's struct and fragment-output
// reflection) all build their search paths through this one helper, so the
// policy lives in a single place.

namespace Veng::Cook
{
    /// @brief Builds a Slang session's search paths from the source dir and the engine include dir.
    ///
    /// The source file's own directory is first so a local file always shadows a same-named
    /// engine file; the engine core shader directory (when non-empty) is second so a consumer
    /// `.slang` resolves `#include "Veng/material.slang"`. The returned vector owns the path
    /// strings; the caller keeps it alive while it holds the returned pointer array.
    /// @param sourceDir           Directory of the .slang source being compiled (always added).
    /// @param engineShaderIncludeDir  Engine core shader dir; skipped when empty (zero-config cook).
    /// @return The search-path strings in resolution order.
    [[nodiscard]] std::vector<std::string>
    BuildSlangSearchPaths(const path& sourceDir, const path& engineShaderIncludeDir);

    /// @brief Sets a SessionDesc's searchPaths/searchPathCount from a BuildSlangSearchPaths result.
    ///
    /// `pointers` is filled with one `const char*` per entry in `paths`, and wired onto `desc`.
    /// Both `paths` and `pointers` must outlive every use of `desc`.
    /// @param desc      The session descriptor to configure.
    /// @param paths     The search-path strings (kept alive by the caller).
    /// @param pointers  Scratch vector receiving the c_str pointers (kept alive by the caller).
    void ApplySlangSearchPaths(slang::SessionDesc& desc, const std::vector<std::string>& paths,
                               std::vector<const char*>& pointers);
}
