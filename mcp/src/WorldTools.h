#pragma once

namespace Veng::Mcp
{
    class McpServer;
    struct McpHost;

    /// @brief Registers the read-only world tools into the server, capturing the host by reference.
    ///
    /// Adds world.list_entities, entity.get, world.query, and scene.stats. Every handler runs
    /// on the render thread during McpServer::Pump() and reads the host's CurrentWorld() scene
    /// through const accessors, so it forces no spatial-version bump. A null CurrentWorld()
    /// yields an empty/so-stated result, never a null deref. The host must outlive the server.
    /// @param server  The server to register the tools into (before its first Pump()).
    /// @param host    The provider seam captured by reference into each handler.
    void RegisterWorldTools(McpServer& server, const McpHost& host);
}
