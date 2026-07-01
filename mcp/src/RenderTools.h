#pragma once

namespace Veng::Mcp
{
    class McpServer;
    struct McpHost;

    /// @brief Registers the render-inspection tools into the server, capturing the host by reference.
    ///
    /// Adds render.screenshot, render.list_viewports, and render.stats. Every handler runs on
    /// the render thread during McpServer::Pump() and resolves its viewport through the host's
    /// Viewport / ViewportNames closures, so a Download() blocks in lockstep with the frame and
    /// no viewport is touched off the render thread. A host that sets neither closure leaves the
    /// render tools reporting "no viewport(s)", never a null deref. The host must outlive the server.
    /// @param server  The server to register the tools into (before its first Pump()).
    /// @param host    The provider seam captured by reference into each handler.
    void RegisterRenderTools(McpServer& server, const McpHost& host);
}
