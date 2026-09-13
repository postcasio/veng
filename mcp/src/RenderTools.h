#pragma once

namespace Veng::Mcp
{
    class McpServer;
    struct McpHost;

    /// @brief Registers the render-inspection tools into the server, capturing the host by reference.
    ///
    /// Adds render.screenshot, render.list_viewports, render.stats and render.capture_status.
    /// Every handler runs on the render thread during McpServer::Pump() and resolves its viewport
    /// through the host's Viewport / ViewportNames closures, so a Download() blocks in lockstep
    /// with the frame and no viewport is touched off the render thread. A host that sets neither
    /// closure leaves the render tools reporting "no viewport(s)", never a null deref. The host
    /// must outlive the server.
    /// @param server  The server to register the tools into (before its first Pump()).
    /// @param host    The provider seam captured by reference into each handler.
    void RegisterRenderTools(McpServer& server, const McpHost& host);

    /// @brief Registers the video-capture control tools (render.capture_start / render.capture_stop).
    ///
    /// Adds the two verbs that begin and end a recording. A capture writes a file and drives the
    /// application's frame clock, so they are registered only when McpServerInfo::AllowMutations is
    /// set — beside the mutation, input and profiler-capture families — and a read-only server
    /// exposes neither, leaving render.capture_status as the whole surface. No argument is ever a
    /// filesystem path: a capture is named and the engine resolves it under the capture directory.
    /// A host that leaves McpHost::VideoRecorder null makes both verbs report the capture
    /// unavailable. The host must outlive the server.
    /// @param server  The server to register the tools into (before its first Pump()).
    /// @param host    The provider seam captured by reference into each handler.
    void RegisterRenderCaptureWriteTools(McpServer& server, const McpHost& host);
}
