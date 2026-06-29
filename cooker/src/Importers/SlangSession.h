#pragma once

#include <optional>
#include <string>
#include <vector>

#include <slang/slang.h>

#include <Veng/Veng.h>

// Cooker-internal Slang session search-path policy. The three session setups
// (ShaderImporter's compile, SlangReflect's struct and fragment-output
// reflection) all build their search paths through this one helper, so the
// policy lives in a single place. A shader whose source is a node graph has no
// .slang file on disk; SlangModuleSource carries either an on-disk path or the
// generated text so one session helper loads both.

namespace Veng::Cook
{
    /// @brief Source of a Slang module: an on-disk .slang file or generated text in memory.
    ///
    /// A graph-sourced fragment shader has no .slang file — the cooker generates its text
    /// from the node graph (see GraphShaderSource) — so the three Slang sessions take this
    /// rather than a bare path. @ref GeneratedSource, when set, is loaded from a string with
    /// @ref Path as its virtual path (so a relative `#include` still resolves through the
    /// session search paths); when unset, @ref Path names the .slang file to load.
    struct SlangModuleSource
    {
        /// @brief Path to the .slang file (file source), or the graph's path (generated source).
        ///
        /// For a file source this is the .slang to load; its directory seeds the search path.
        /// For a generated source this is the *.graph.json path: its directory seeds the search
        /// path and its stem names the in-memory module.
        path Path;
        /// @brief Generated Slang text; when set, the module is loaded from this string.
        std::optional<std::string> GeneratedSource;
    };

    /// @brief Loads a Slang module from a SlangModuleSource into a session.
    ///
    /// Dispatches to `loadModule` for a file source or `loadModuleFromSourceString` for a
    /// generated one, returning the module or nullptr (with diagnostics written through
    /// @p outDiagnostics) on failure — the caller formats the located error.
    /// @param session       The Slang session to load into.
    /// @param source        The module source (file path or generated text).
    /// @param outDiagnostics Receives compile diagnostics on failure.
    /// @return The loaded module, or nullptr on failure.
    [[nodiscard]] slang::IModule* LoadSlangModule(slang::ISession& session,
                                                  const SlangModuleSource& source,
                                                  slang::IBlob** outDiagnostics);
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
