#pragma once

namespace Veng::Mcp
{
    class McpServer;
    struct McpHost;

    /// @brief Registers the input-injection tools into the server, capturing the host by reference.
    ///
    /// Adds input.send — an ordered batch of synthetic input events (key/mouse-button down and up,
    /// mouse move, scroll, and a UTF-8 text run typed one character event per codepoint) fed into
    /// the running app so an agent can drive it. Registered only when
    /// McpServerInfo::AllowMutations is set — injecting input mutates app state, so a read-only
    /// server exposes none of it and tools/list honestly reflects the server's write capability.
    ///
    /// The handler runs on the render thread during McpServer::Pump(), at the mutation-safe pump
    /// point, and applies each event through McpHost::InjectInput, so an injected event lands
    /// before the frame's action resolution reads input exactly as a real window event does. It
    /// validates the batch shape up front (a non-empty array within MaxInputBatchSize, each event
    /// well-formed) and rejects a structural error as the whole call. A host that leaves
    /// InjectInput null makes the tool report that injection is unavailable. The host must outlive
    /// the server.
    /// @param server  The server to register the tools into (before its first Pump()).
    /// @param host    The provider seam captured by reference into each handler.
    void RegisterInputTools(McpServer& server, const McpHost& host);
}
