#pragma once

namespace Veng::Mcp
{
    class McpServer;
    struct McpHost;

    /// @brief Registers the read-only profiler tool (profile.stats) into the server.
    ///
    /// Adds profile.stats — the live per-scope aggregates for the last completed frame plus the
    /// profiler state and drop counters, reading nothing off disk and writing nothing. Always
    /// registered (like the render tools), so a default read-only server still answers "is this
    /// frame slow, and where". Each handler runs on the render thread during McpServer::Pump() and
    /// reaches the profiler through McpHost::Profiler; a host that leaves it null makes the tool
    /// report the profiler unavailable. The host must outlive the server.
    /// @param server  The server to register the tool into (before its first Pump()).
    /// @param host    The provider seam captured by reference into each handler.
    void RegisterProfileReadTools(McpServer& server, const McpHost& host);

    /// @brief Registers the capture-control tools (profile.start / profile.stop / profile.dump_ring).
    ///
    /// Adds the three verbs that begin, end, and dump a capture. Each writes a trace file to disk, so
    /// they are registered only when McpServerInfo::AllowMutations is set — alongside the mutation and
    /// input families — and a read-only server exposes none of them, keeping tools/list truthful. No
    /// argument is ever a filesystem path: the tools name a capture and the engine resolves it under
    /// the capture directory; the written path travels outward in the response. A host that leaves
    /// McpHost::Profiler null makes each verb report the profiler unavailable. The host must outlive
    /// the server.
    /// @param server  The server to register the tools into (before its first Pump()).
    /// @param host    The provider seam captured by reference into each handler.
    void RegisterProfileWriteTools(McpServer& server, const McpHost& host);
}
