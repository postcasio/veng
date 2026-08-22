#pragma once

namespace Veng::Mcp
{
    class McpServer;
    struct McpHost;

    /// @brief Registers the read-only GUI inspection tools into the server, capturing the host by
    ///        reference.
    ///
    /// Adds gui.list_documents and gui.inspect. A rendered interface is the one part of a scene
    /// that cannot be read back through the entity tools: the element tree is owned by a
    /// `Gui::Document` behind a surface or an overlay component, and its solved rects exist only
    /// after a layout pass. Without these, the only way to ask where an interface actually put
    /// something is to capture the frame and measure pixels.
    ///
    /// Both handlers run on the render thread during McpServer::Pump() and read the document
    /// through const accessors, so neither drives a solve nor dirties one. A null CurrentWorld(),
    /// a scene with no document, or a document with no root all yield an empty result rather than
    /// a null deref. The host must outlive the server.
    /// @param server  The server to register the tools into (before its first Pump()).
    /// @param host    The provider seam captured by reference into each handler.
    void RegisterGuiTools(McpServer& server, const McpHost& host);
}
