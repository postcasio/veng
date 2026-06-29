#pragma once

#include <Veng/Asset/Material.h>
#include <Veng/Cook/Types.h>

// Cooker-internal helper resolving a fragment shader whose source is a node
// graph rather than a .slang file. A graph-sourced *.shader.json names a
// *.graph.json (the authored node graph) and a domain; this helper reads the
// graph, runs the shared veng::graph emit walk (CompileMaterialGraph), and
// returns the generated Slang fragment source — the same text the editor
// preview generates, so the offline cook and the live preview agree by
// construction. The ShaderImporter compiles the text; the MaterialImporter
// reflects MaterialParams and the fragment outputs from it. A hand-authored
// .slang source is left untouched (IsGraph false).

namespace Veng::Cook
{
    /// @brief The resolved source of a fragment shader: a hand-authored .slang or a
    /// graph the cooker generated Slang text from.
    struct GraphShaderSource
    {
        /// @brief True when the shader's source is a node graph (generated text in @ref Source).
        bool IsGraph = false;
        /// @brief The generated Slang fragment source; meaningful only when IsGraph is true.
        string Source;
        /// @brief Absolute path to the authored *.graph.json; meaningful only when IsGraph is true.
        path GraphPath;
        /// @brief The material domain the graph was generated for; Surface for a non-graph source.
        MaterialDomain Domain = MaterialDomain::Surface;
    };

    /// @brief Resolves whether a parsed *.shader.json names a graph source, generating its Slang.
    ///
    /// A graph-sourced entry names a `"source"` ending in `.graph.json` and a
    /// `"domain"` (`"surface"` | `"postprocess"`, default surface). The helper reads the
    /// graph relative to @p shaderJsonDir, runs the shared emit walk, and returns the
    /// generated Slang text. A `.slang` source returns `{ IsGraph = false }` and the caller
    /// compiles the file as today.
    /// @param shaderJson    The parsed *.shader.json document.
    /// @param shaderJsonDir Directory of the *.shader.json (the `"source"` path is relative to it).
    /// @return The resolved source, or a located error on a malformed graph or a failed walk.
    [[nodiscard]] Result<GraphShaderSource> ResolveGraphShaderSource(const json& shaderJson,
                                                                     const path& shaderJsonDir);

    /// @brief Signature of the graph-source resolver hook the shader/material importers call.
    using GraphShaderResolver =
        function<Result<GraphShaderSource>(const json& shaderJson, const path& shaderJsonDir)>;

    /// @brief Installs the graph-source resolver the shader/material importers call.
    ///
    /// The emit walk lives in veng::graph, linked only into libveng_cook (never the veng-free
    /// core-pack bootstrap, which cooks no graph sources). RegisterBuiltinImporters installs
    /// ResolveGraphShaderSource here so the shader/material importers — compiled into the
    /// veng-free object library — reach the walk without statically depending on it.
    /// @param resolver The resolver to install.
    void SetGraphShaderResolver(GraphShaderResolver resolver);

    /// @brief Resolves a graph shader source through the installed hook.
    ///
    /// Returns `{ IsGraph = false }` when no resolver is installed (the bootstrap path): a
    /// graph-sourced shader is then unreachable, exactly as it is unused there.
    /// @param shaderJson    The parsed *.shader.json document.
    /// @param shaderJsonDir Directory of the *.shader.json.
    /// @return The resolved source, or a located error.
    [[nodiscard]] Result<GraphShaderSource> ResolveGraphShaderSourceHook(const json& shaderJson,
                                                                         const path& shaderJsonDir);
}
