#pragma once

#include <Veng/Veng.h>
#include <Veng/Result.h>
#include <Veng/Mcp/McpClientInfo.h>

namespace Veng::Mcp
{
    /// @brief One advertised tool, as returned by tools/list.
    struct McpToolDesc
    {
        /// @brief The noun.verb tool name.
        string Name;

        /// @brief Human-readable summary (from the tool's schema).
        string Description;

        /// @brief The tool's inputSchema as a JSON string (verbatim).
        string InputSchema;
    };

    /// @brief An image content block from a tools/call result.
    ///
    /// A tool like render.screenshot returns an image content block rather than text.
    /// McpCallResult surfaces the first such block here so a caller can write it out;
    /// any further image blocks are dropped.
    struct McpImageBlock
    {
        /// @brief The image's MIME type (e.g. "image/png").
        string MimeType;

        /// @brief The image data, base64-encoded exactly as the server returned it.
        string Base64Data;
    };

    /// @brief The outcome of one tools/call: the content payload plus the tool-error flag.
    ///
    /// A successful Result carrying an McpCallResult means the call completed at the
    /// protocol level. IsError distinguishes a tool-level failure (the server set
    /// result.isError) from a normal result — a distinct failure shape from a JSON-RPC
    /// protocol error, which is instead the Result's error().
    struct McpCallResult
    {
        /// @brief The concatenated text content blocks (the tool's JSON payload).
        string Content;

        /// @brief True when the server flagged result.isError — a tool-level failure.
        bool IsError = false;

        /// @brief The first image content block, if the result carried one.
        optional<McpImageBlock> Image;
    };

    /// @brief A loopback MCP client that drives a running veng MCP server.
    ///
    /// The reusable transport half beside McpServer: it opens a request/response
    /// connection to a running server, calls one tool or lists the tools, and hands
    /// back a Result. The server is stateless — one POST / per call, a single
    /// application/json reply, no Mcp-Session-Id, no SSE stream — so the client is a
    /// bare request/response object with no session handshake to open and nothing to
    /// tear down.
    ///
    /// The client issues no `initialize` call: the server does not require one before
    /// tools/call, so issuing one would be a wasted round-trip. It negotiates no
    /// protocol version and reads no SSE, so a server that required a handshake,
    /// advertised an incompatible protocol, or replied with a stream would surface here
    /// as an ordinary rpc/connection error, not a graceful degrade. It sends no `Origin`
    /// header, so it stays acceptable to the server's Origin-rejection defense.
    ///
    /// It is Unique, single-owner; Create(const McpClientInfo&) is the factory.
    class VE_API McpClient
    {
    public:
        /// @brief Constructs a client for the target described by @p info.
        ///
        /// Forms the loopback connection lazily: an actual connection failure surfaces on
        /// the first ListTools/CallTool, not here. A client that cannot even be formed is
        /// the Result error.
        /// @param info  The client descriptor (host, port, timeout).
        /// @return The owned client, or a located error.
        static Result<Unique<McpClient>> Create(const McpClientInfo& info);

        /// @brief Closes the connection (RAII — that is the whole of cleanup).
        ~McpClient();

        McpClient(const McpClient&) = delete;
        McpClient& operator=(const McpClient&) = delete;

        /// @brief Lists the server's advertised tools (tools/list). One POST, one parse.
        ///
        /// A transport failure (connection refused, timeout) or a JSON-RPC `error` object
        /// is the Result's error(); otherwise the returned tools are mapped one-for-one.
        /// @return The advertised tools, or a located error.
        [[nodiscard]] Result<vector<McpToolDesc>> ListTools();

        /// @brief Calls one tool (tools/call) with a JSON arguments object string.
        ///
        /// A JSON-RPC protocol error (or a transport failure) is the Result's error(); a
        /// tool-level failure is a successful Result carrying McpCallResult{ .IsError =
        /// true }. The two failure shapes stay distinguishable at this layer.
        /// @param name           The tool to call (noun.verb).
        /// @param argumentsJson  The tool's `arguments` object, as a JSON string.
        /// @return The call outcome, or a located error.
        [[nodiscard]] Result<McpCallResult> CallTool(string_view name, string_view argumentsJson);

        /// @brief Opaque backend state (the httplib client).
        ///
        /// Forward-declared here and defined in the implementation TU (the Native idiom),
        /// so no backend type leaks into this public header.
        struct Native;

    private:
        McpClient();

        /// @brief Backend state, owned single-owner and torn down in the destructor.
        Unique<Native> m_Native;
    };
}
