#pragma once

namespace Veng::Mcp
{
    class McpServer;
    struct McpHost;

    /// @brief Registers the read-only haptics tool (haptics.state) into the server.
    ///
    /// Adds haptics.state — each pad's mixed rumble output, the master intensity, whether the output
    /// is suspended, and every live rumble instance. Always registered (like audio.list_voices), so a
    /// driven session can confirm a clip plays on a virtual pad with no hardware. The handler runs on
    /// the render thread during McpServer::Pump() and reaches the engine through McpHost::Haptics; a
    /// host that leaves it null makes the tool report haptics unavailable. The host must outlive the
    /// server.
    /// @param server  The server to register the tool into (before its first Pump()).
    /// @param host    The provider seam captured by reference into the handler.
    void RegisterHapticsTools(McpServer& server, const McpHost& host);
}
