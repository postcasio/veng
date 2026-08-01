#pragma once

#include <Veng/Veng.h>
#include <Veng/Mcp/McpServerInfo.h>
#include <Veng/Mcp/McpHost.h>
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
    /// - Pump() runs on the render thread; it is where a tool handler executes unless the
    ///   tool declares otherwise, so a handler may freely touch engine state. A handler must
    ///   not block on another MCP request (no re-entrancy).
    /// - A tool that declares McpTool::RunsOffPump is the one exception: its OffPumpHandler
    ///   runs on the network thread that received the request and must touch no engine state
    ///   at all. Its optional McpTool::PumpedPrologue still runs at the pump point, so the
    ///   snapshot such a handler reads is taken where reading it is safe. At most two
    ///   off-pump handlers run at once and never two for one tool; a call exceeding either
    ///   bound is refused with a located off-pump-busy error rather than queued.
    /// - The network thread touches only the immutable tool registry (for `tools/list`), the
    ///   request queue, and an off-pump handler that declared it needs no engine state.
    ///
    /// It is Unique, single-owner: dropping the Unique stops the listener thread and
    /// closes the socket (RAII — that is the whole of cleanup).
    class VE_API McpServer
    {
    public:
        /// @brief Constructs a server, binds the socket, and registers the built-in tools.
        ///
        /// The bound address and resolved port are logged at Info once the socket is
        /// listening. A Port of 0 resolves to an ephemeral port readable via GetPort(). The
        /// read-only world tools (world.list_entities, entity.get, world.query, scene.stats)
        /// auto-register from @p host, so a consumer that just links veng::mcp and constructs a
        /// server gets them. The host is captured by reference and must outlive the server.
        /// @param info  The server descriptor.
        /// @param host  The provider seam the built-in tools reach live state through.
        /// @return The owned server.
        static Unique<McpServer> Create(const McpServerInfo& info, const McpHost& host);

        /// @brief Stops the listener thread, closes the socket, and drains in-flight requests.
        ///
        /// Every queued and waiting request resolves with a shutdown error so its network
        /// thread unblocks. An off-pump handler already running is in no queue, so it is
        /// signalled through McpOffPumpRequest::IsCancelled instead: teardown then costs the
        /// handler's poll interval rather than its remaining runtime.
        ~McpServer();

        McpServer(const McpServer&) = delete;
        McpServer& operator=(const McpServer&) = delete;

        /// @brief Registers a tool, surfaced to clients via `tools/list` and `tools/call`.
        ///
        /// Called on the render thread at construction, before the server serves — not
        /// concurrently with Pump(). Asserts fatally on a duplicate tool name, and on a
        /// threading declaration that does not hold together: a tool must supply exactly one
        /// of McpTool::Handler and McpTool::OffPumpHandler, matching its McpTool::RunsOffPump
        /// flag, and may declare an McpTool::PumpedPrologue only when it runs off the pump.
        /// @param tool  The tool to register.
        void RegisterTool(McpTool tool);

        /// @brief Drains the render-thread request queue, running each pending tool handler.
        ///
        /// For each pending `tools/call`, runs the tool handler on the calling thread,
        /// stores its result, and wakes the blocked network thread. Called once per frame
        /// by the owner at a scene-safe point (before any scene iteration). An off-pump
        /// tool's pumped prologue is queued and run here too; its handler is not.
        void Pump();

        /// @brief Returns the bound port (resolves a requested Port of 0 to the actual one).
        [[nodiscard]] u16 GetPort() const;

        /// @brief Opaque backend state (the httplib server, JSON machinery, thread, queue).
        ///
        /// Forward-declared here and defined in the implementation TU (the Native idiom),
        /// so no backend or JSON type leaks into this public header.
        struct Native;

    private:
        McpServer();

        /// @brief Backend state, owned single-owner and torn down in the destructor.
        Unique<Native> m_Native;
    };
}
