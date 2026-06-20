#pragma once

#include <Veng/Veng.h>
#include <Veng/Result.h>

#include <VengEditor/NodeGraph/NodeGraph.h>
#include <VengEditor/NodeGraph/NodeType.h>

namespace VengEditor
{
    /// @brief Format version carried under "version" in the serialized graph document.
    ///
    /// A read whose version exceeds this returns VersionTooNew, so a degraded parse
    /// never overwrites the author's data.
    inline constexpr Veng::i32 NodeGraphFormatVersion = 1;

    /// @brief Serializes a graph to a JSON document string.
    ///
    /// Each node is written with its catalog type (by stable name), canvas position,
    /// and property values; each link by endpoint node index and pin name.
    /// The JSON library type is a PRIVATE dependency of libveng_editor; the public
    /// surface is a plain string.
    /// @param graph   The graph to serialize.
    /// @param catalog Catalog used to resolve node type names and pin descs.
    /// @return A JSON object string carrying NodeGraphFormatVersion under "version".
    [[nodiscard]] Veng::string WriteNodeGraph(const NodeGraph& graph, const NodeCatalog& catalog);

    /// @brief Result kind of a graph read.
    ///
    /// Loaded covers both a clean parse and a tolerant one (unknown node types or
    /// dangling links dropped with a warning).
    enum class NodeGraphReadOutcome : Veng::u8
    {
        /// @brief Graph was read into the destination (possibly with dropped nodes/links).
        Loaded,
        /// @brief "version" exceeds NodeGraphFormatVersion; nothing was written.
        VersionTooNew,
        /// @brief The string is not a recognisable graph document.
        Malformed,
    };

    /// @brief Reads a graph from a JSON document string into a fresh destination graph.
    ///
    /// The destination must be constructed against the catalog's hooks. Tolerant within
    /// a supported version: unknown node types and dangling links are dropped and
    /// logged. A version newer than NodeGraphFormatVersion returns VersionTooNew and
    /// writes nothing, allowing the panel to open read-only.
    /// @param json    Source JSON document string.
    /// @param dest    Fresh graph to populate; must be constructed against catalog.
    /// @param catalog Catalog used to resolve node types and pin descs by name.
    /// @return The read outcome.
    [[nodiscard]] NodeGraphReadOutcome ReadNodeGraph(Veng::string_view json, NodeGraph& dest,
                                                     const NodeCatalog& catalog);
}
