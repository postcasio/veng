#pragma once

namespace Veng::Mcp
{
    class McpServer;
    struct McpHost;

    /// @brief Registers the scene-mutation tools into the server, capturing the host by reference.
    ///
    /// Adds entity.add_component, entity.remove_component, entity.set_field, entity.spawn,
    /// entity.destroy, and world.load_prefab. Registered only when McpServerInfo::AllowMutations
    /// is set — a read-only server exposes none of them, so tools/list honestly reflects the
    /// server's write capability.
    ///
    /// Every handler runs on the render thread during McpServer::Pump(), outside any View/Each
    /// iteration, so a structural edit lands at the mutation-safe pump point. Each validates its
    /// target entity (a stale generational id is a located error, not silent UB) and either hands
    /// its McpMutation to McpHost::ApplyMutation (an editor host routes it through the CommandStack
    /// for undo) or, when that hook is null, applies the edit raw to CurrentWorld(). The host must
    /// outlive the server.
    /// @param server  The server to register the tools into (before its first Pump()).
    /// @param host    The provider seam captured by reference into each handler.
    void RegisterMutationTools(McpServer& server, const McpHost& host);
}
