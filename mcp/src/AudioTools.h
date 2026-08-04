#pragma once

namespace Veng::Mcp
{
    class McpServer;
    struct McpHost;

    /// @brief Registers the read-only audio tool (audio.list_voices) into the server.
    ///
    /// Adds audio.list_voices — the live voice table of the presented world's mix: each voice's bus,
    /// gain, pan/pitch, spatial pose, whether it is a clip or a generator, and its looping flag, plus
    /// the music director's current track and gain. Always registered (like the render tools), so a
    /// default read-only server can answer "what is playing?" without a speaker. The handler runs on
    /// the render thread during McpServer::Pump() and reaches the engine through McpHost::Audio; a
    /// host that leaves it null makes the tool report audio unavailable. The host must outlive the
    /// server.
    /// @param server  The server to register the tool into (before its first Pump()).
    /// @param host    The provider seam captured by reference into the handler.
    void RegisterAudioTools(McpServer& server, const McpHost& host);
}
