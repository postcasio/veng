#pragma once

#include <Veng/Veng.h>
#include <Veng/Result.h>

#include <VengEditor/NodeGraph/NodeGraph.h>
#include <VengEditor/NodeGraph/NodeType.h>

namespace VengEditor
{
    // The serialized graph document format version, carried under "version" in
    // the JSON object. A read whose version exceeds this is refused (Outcome::
    // VersionTooNew) so a degraded parse never overwrites the author's data.
    inline constexpr Veng::i32 NodeGraphFormatVersion = 1;

    // Serializes a graph to a JSON document string: each node's catalog type (by
    // stable name), canvas position, and property values (a per-FieldClass JSON
    // walker over the type's FieldDescriptors); each link by endpoint node + pin
    // name. Carries NodeGraphFormatVersion under "version". The string is a JSON
    // object the panel parses and embeds under "_editor" in the .vmat.json — the
    // public surface stays free of the JSON library type (it is a PRIVATE
    // dependency of libveng_editor).
    [[nodiscard]] Veng::string WriteNodeGraph(const NodeGraph& graph, const NodeCatalog& catalog);

    // The result kind of a graph read. Loaded covers both a clean parse and a
    // tolerant one (unknown node types / dangling links dropped with a warning).
    enum class NodeGraphReadOutcome : Veng::u8
    {
        Loaded,        // the graph was read into the destination
        VersionTooNew, // "version" exceeds NodeGraphFormatVersion; nothing read
        Malformed,     // the string is not a recognisable graph document
    };

    // Reads a graph from a JSON document string into a fresh destination graph
    // (which must be constructed against catalog's hooks). Tolerant within a
    // supported version: an unknown node type or a dangling link is dropped and
    // logged, not fatal. A version newer than NodeGraphFormatVersion returns
    // VersionTooNew and writes nothing, so the panel can open read-only rather
    // than regenerate from a degraded parse.
    [[nodiscard]] NodeGraphReadOutcome ReadNodeGraph(Veng::string_view json, NodeGraph& dest,
                                                     const NodeCatalog& catalog);
}
