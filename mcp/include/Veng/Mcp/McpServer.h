#pragma once

#include <Veng/Veng.h>
#include <Veng/Mcp/McpServerInfo.h>
#include <Veng/Mcp/McpTool.h>

namespace Veng::Mcp
{
    /// @brief A loopback MCP server exposing registered tools to an AI agent.
    ///
    /// Owns a Streamable-HTTP transport on a background network thread, a JSON-RPC 2.0
    /// dispatch implementing the MCP `initialize` / `tools/list` / `tools/call` methods,
    /// a tool registry, and its own render-thread request queue drained by Pump().
    ///
    /// Threading contract:
    /// - RegisterTool is called on the render thread at construction, before the server
    ///   serves engine tools — never concurrently with Pump().
    /// - Pump() runs on the render thread; it is the only place a tool handler executes,
    ///   so a handler may freely touch engine state. A handler must not block on another
    ///   MCP request (no re-entrancy).
    /// - The network thread touches only the immutable tool registry (for `tools/list`)
    ///   and the request queue; it never touches engine state.
    ///
    /// It is Unique, single-owner: dropping the Unique stops the listener thread and
    /// closes the socket (RAII — that is the whole of cleanup).
    class VE_API McpServer
    {
    public:
        /// @brief Constructs a server, binds the socket, and starts the network thread.
        ///
        /// The bound address and resolved port are logged at Info once the socket is
        /// listening. A Port of 0 resolves to an ephemeral port readable via GetPort().
        /// @param info  The server descriptor.
        /// @return The owned server.
        static Unique<McpServer> Create(const McpServerInfo& info);

        /// @brief Stops the listener thread, closes the socket, and drains in-flight requests.
        ~McpServer();

        McpServer(const McpServer&) = delete;
        McpServer& operator=(const McpServer&) = delete;

        /// @brief Registers a tool, surfaced to clients via `tools/list` and `tools/call`.
        ///
        /// Called on the render thread at construction, before the server serves — not
        /// concurrently with Pump(). Asserts fatally on a duplicate tool name.
        /// @param tool  The tool to register.
        void RegisterTool(McpTool tool);

        /// @brief Drains the render-thread request queue, running each pending tool handler.
        ///
        /// For each pending `tools/call`, runs the tool handler on the calling thread,
        /// stores its result, and wakes the blocked network thread. Called once per frame
        /// by the owner at a scene-safe point (before any scene iteration).
        void Pump();

        /// @brief Returns the bound port (resolves a requested Port of 0 to the actual one).
        [[nodiscard]] u16 GetPort() const;

        /// @brief Opaque backend state (the httplib server, JSON machinery, thread, queue).
        ///
        /// Forward-declared here and defined in the implementation TU (the Native idiom),
        /// so no backend or JSON type leaks into this public header.
        struct Native;

    private:
        McpServer() = default;

        /// @brief Backend state, owned single-owner and torn down in the destructor.
        Unique<Native> m_Native;
    };
}
