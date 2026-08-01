#pragma once

#include <Veng/Veng.h>
#include <Veng/Result.h>

namespace Veng::Mcp
{
    /// @brief What an off-pump handler receives in place of the pumped handler's arguments.
    ///
    /// Bundles everything an McpTool::OffPumpHandler is allowed to read: the request's own
    /// arguments, the snapshot its pumped prologue took, and the cancellation the server
    /// signals at teardown. Everything else — the scene, the renderer, the asset manager —
    /// belongs to the render thread and is out of reach by contract.
    struct McpOffPumpRequest
    {
        /// @brief The `arguments` object as a JSON string.
        string_view Arguments;

        /// @brief The pumped prologue's result, or empty when the tool declares no prologue.
        string_view Snapshot;

        /// @brief Reports whether the server has begun tearing down.
        ///
        /// Cooperative cancellation: an off-pump handler runs on a network thread the
        /// destructor has to join, so teardown waits for whatever the handler is doing. Poll
        /// this every few milliseconds and return promptly once it is set — with a partial
        /// result or a located error, whichever is honest. A handler that never polls turns
        /// shutdown into a hang of its own remaining length.
        function<bool()> IsCancelled;
    };

    /// @brief A tool registered with an McpServer and surfaced to MCP clients.
    ///
    /// The library is JSON-library-free at its surface: a handler receives its
    /// arguments as a JSON string and returns a JSON string (the tool result
    /// payload) or a located error, and the server parses/serializes internally.
    ///
    /// A tool runs at the pump point unless it says otherwise: Handler executes on the render
    /// thread during McpServer::Pump(), never on the network thread, so it may freely touch
    /// engine state. A tool whose work is a pure computation over data it owns may instead
    /// declare RunsOffPump and supply OffPumpHandler, which runs on the network thread under
    /// the narrow contract stated there — with an optional PumpedPrologue taking whatever
    /// snapshot of engine state that computation needs, at the pump point where reading it is
    /// safe.
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

        /// @brief Whether the handler's returned string is a pre-formed content array.
        ///
        /// A plain tool (the default) returns a JSON payload the server wraps in a single
        /// text content block. A content-block tool returns the tool result's `content`
        /// array itself as a JSON string (e.g. an image block from render.screenshot); the
        /// server splices it in verbatim rather than nesting it inside a text block. A
        /// located error is still surfaced as an isError text result regardless.
        bool ReturnsContentBlocks = false;

        /// @brief Whether the handler runs on the network thread instead of at the pump point.
        ///
        /// False — the default, and the safe one — puts the tool on the pumped path: Handler
        /// runs on the render thread during Pump(), where engine state is coherent. Setting
        /// it is a declaration that the work needs none of that: the server then runs
        /// OffPumpHandler on the network thread that received the request, so the tool
        /// neither holds the frame open for its duration nor is bounded by how soon the
        /// render thread pumps. A tool that sets it supplies OffPumpHandler and leaves
        /// Handler empty; the two are checked against each other at registration.
        bool RunsOffPump = false;

        /// @brief Optional snapshot the server takes at the pump point for an off-pump handler.
        ///
        /// Runs on the render thread during Pump(), under the pumped path's rules and its
        /// request timeout, so it may freely touch engine state — and is expected to take
        /// microseconds. It exists for the one shape the flag alone cannot express: a long
        /// pure computation over a small copy of engine state, where the copy is taken where
        /// it is safe and the computation leaves. The returned JSON string reaches the
        /// handler as McpOffPumpRequest::Snapshot; a located error is reported to the client
        /// and the handler does not run.
        ///
        /// Meaningful only with RunsOffPump. The server invokes it, never the handler, so an
        /// off-pump handler cannot reach back to the pump mid-flight and the no-re-entrancy
        /// rule holds.
        function<Result<string>()> PumpedPrologue;

        /// @brief Runs the tool on the network thread and produces its result.
        ///
        /// Used when RunsOffPump is set, in place of Handler, and bound by a narrow contract:
        /// it may read its arguments, its prologue's snapshot, and data it builds itself, and
        /// it may touch **no** engine state whatever — not the scene, the renderer, the asset
        /// manager, or anything else the render thread owns. Getting that wrong is a data
        /// race, which presents as an unrelated intermittent failure somewhere else entirely.
        ///
        /// It must poll McpOffPumpRequest::IsCancelled every few milliseconds and return
        /// promptly once it is set, because the server's teardown waits for it.
        ///
        /// The server runs at most two off-pump handlers at once, and never two for the same
        /// tool; a call that would exceed either bound is refused with a located off-pump-busy
        /// error rather than queued behind them. Return and error conventions are Handler's:
        /// the payload (or, under ReturnsContentBlocks, the `content` array) as a JSON string,
        /// or a located error carrying the reason alone.
        function<Result<string>(const McpOffPumpRequest& request)> OffPumpHandler;

        /// @brief Runs the tool on the render thread and produces its result.
        ///
        /// Receives the `arguments` object as a JSON string and returns the tool
        /// result payload as a JSON string, or a located error surfaced to the
        /// client as an MCP `isError` tool result (not a JSON-RPC protocol error).
        /// When ReturnsContentBlocks is set, the returned string is the `content`
        /// array itself. Must not block on another MCP request (no re-entrancy).
        ///
        /// A located error carries the **reason alone**: the server prefixes Name to it
        /// when it assembles the isError result, so a handler that also names itself
        /// makes the tool appear twice in the reported error.
        function<Result<string>(string_view argsJson)> Handler;
    };
}
