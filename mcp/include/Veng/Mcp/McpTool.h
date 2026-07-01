#pragma once

#include <Veng/Veng.h>
#include <Veng/Result.h>

namespace Veng::Mcp
{
    /// @brief A tool registered with an McpServer and surfaced to MCP clients.
    ///
    /// The library is JSON-library-free at its surface: a handler receives its
    /// arguments as a JSON string and returns a JSON string (the tool result
    /// payload) or a located error, and the server parses/serializes internally.
    /// The handler runs on the render thread during McpServer::Pump(), never on
    /// the network thread, so it may freely touch engine state.
    struct McpTool
    {
        /// @brief Unique tool name (the `noun.verb` / `noun.property` convention).
        string Name;

        /// @brief Human-readable one-line description surfaced verbatim in `tools/list`.
        string Description;

        /// @brief JSON-schema string describing the tool's arguments object.
        ///
        /// Surfaced verbatim as the tool's `inputSchema` in `tools/list`. An empty
        /// string is serialized as an empty object schema.
        string InputSchemaJson;

        /// @brief Runs the tool on the render thread and produces its result.
        ///
        /// Receives the `arguments` object as a JSON string and returns the tool
        /// result payload as a JSON string, or a located error surfaced to the
        /// client as an MCP `isError` tool result (not a JSON-RPC protocol error).
        /// Must not block on another MCP request (no re-entrancy).
        function<Result<string>(string_view argsJson)> Handler;
    };
}
