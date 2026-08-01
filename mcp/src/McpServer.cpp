#include <Veng/Mcp/McpServer.h>

#include <Veng/Assert.h>
#include <Veng/Log.h>

#include "InputTools.h"
#include "MutationTools.h"
#include "ProfileTools.h"
#include "RenderTools.h"
#include "WorldTools.h"

#include <nlohmann/json.hpp>

// httplib appears here only, never in a public header — it is a PRIVATE dependency
// hidden behind the Native idiom. Its throws are contained in its own -fexceptions
// aggregation TU (Vendor/HttpLib.cpp); this TU stays -fno-exceptions.
#include "Vendor/httplib.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace Veng::Mcp
{
    using Json = nlohmann::json;

    namespace
    {
        /// @brief MCP protocol version this server implements.
        constexpr const char* ProtocolVersion = "2025-06-18";

        /// @brief Server version reported in the `initialize` handshake.
        constexpr const char* ServerVersion = "0.1.0";

        /// @brief Bound on how long a network thread waits for the render thread to pump.
        ///
        /// A synchronous main-thread modal (a native file dialog, a debugger breakpoint)
        /// holds the render thread so Pump() never runs; on expiry the network thread
        /// returns a host-busy error rather than blocking forever.
        constexpr std::chrono::seconds RequestTimeout{5};

        /// @brief How many off-pump handlers the host runs at once.
        ///
        /// The vendored transport's connection pool would otherwise set this, which is no
        /// guarantee at all: eight concurrent expensive reads reached from a socket are a
        /// denial of service against the app's own CPU. The host bounds it instead, and a
        /// call over the bound is refused rather than queued behind the ones running.
        constexpr usize MaxOffPumpInFlight = 2;

        /// @brief JSON-RPC error code: the request body was not valid JSON.
        constexpr int ErrorParse = -32700;
        /// @brief JSON-RPC error code: the message was not a valid JSON-RPC request.
        constexpr int ErrorInvalidRequest = -32600;
        /// @brief JSON-RPC error code: the requested method does not exist.
        constexpr int ErrorMethodNotFound = -32601;
    }

    /// @brief A unit of render-thread work awaiting service at the pump point.
    ///
    /// Either a pumped tool's handler or an off-pump tool's prologue; both wait the same
    /// bounded window for Pump() and report the same host-busy error on expiry.
    ///
    /// Mirrors TaskSystem's TaskState handshake (a mutex + condition_variable + a Done
    /// flag + a Result slot) rather than std::promise/future, whose misuse paths throw
    /// and so are illegal under -fno-exceptions (the shutdown drain is such a path).
    struct PendingRequest
    {
        /// @brief The tool handler to run on the render thread.
        function<Result<string>(string_view)> Handler;
        /// @brief The `arguments` object as a JSON string, passed to the handler.
        string Arguments;

        /// @brief Guards Done and Value.
        std::mutex Mutex;
        /// @brief Signalled when Done becomes true.
        std::condition_variable Ready;
        /// @brief True once Pump() (or the shutdown drain) has written Value.
        bool Done = false;
        /// @brief The handler result, valid when Done.
        Result<string> Value = Result<string>(string{});
    };

    struct McpServer::Native
    {
        McpServerInfo Info;

        /// @brief Immutable once serving: read off-thread by `tools/list` without a lock.
        map<string, McpTool> Tools;

        httplib::Server Http;
        std::thread Thread;

        /// @brief Resolved bind address, held for the readiness log at thread start.
        string Host;
        /// @brief The actual bound port (resolves a requested Port of 0).
        u16 BoundPort = 0;
        /// @brief True once the listener thread has been started (by the first Pump()).
        bool Started = false;

        /// @brief Guards Queue and ShuttingDown.
        std::mutex QueueMutex;
        /// @brief Pending tools/call requests awaiting Pump().
        std::deque<Ref<PendingRequest>> Queue;
        /// @brief Set in the dtor so a late enqueue fails fast instead of blocking forever.
        bool ShuttingDown = false;

        /// @brief Guards OffPumpInFlight. Never taken while QueueMutex is held.
        std::mutex OffPumpMutex;

        /// @brief The tools whose off-pump handler is running right now.
        ///
        /// Bounded by MaxOffPumpInFlight, and holding a name at most once — an off-pump tool
        /// answers one call at a time, so a handler is never re-entered concurrently.
        vector<string> OffPumpInFlight;

        /// @brief Set in the dtor; what McpOffPumpRequest::IsCancelled reports.
        ///
        /// An off-pump handler is in no queue the dtor can drain, and joining the listener
        /// joins the connection pool it runs on — so teardown would otherwise block for the
        /// handler's whole remaining duration.
        std::atomic<bool> Cancelled{false};

        /// @brief Starts the listener thread on the first call, then a no-op.
        void EnsureStarted();

        /// @brief Reserves one of the bounded off-pump slots for @p tool.
        ///
        /// @param tool  The tool whose handler is about to run.
        /// @return False when MaxOffPumpInFlight handlers are already running, or when this
        ///         tool's own handler is — which the caller reports as off-pump-busy rather
        ///         than queueing behind them.
        bool TryAcquireOffPump(const string& tool);

        /// @brief Releases the off-pump slot TryAcquireOffPump reserved for @p tool.
        void ReleaseOffPump(const string& tool);
    };

    namespace
    {
        /// @brief Builds a JSON-RPC error response object for the given id and code.
        Json MakeError(const Json& id, int code, const string& message)
        {
            return Json{
                {"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
        }

        /// @brief Builds a JSON-RPC result response object for the given id.
        Json MakeResult(const Json& id, Json result)
        {
            return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
        }

        /// @brief Parses a tool's input-schema string into a JSON object, defaulting to `{}`.
        Json ParseInputSchema(const string& schema)
        {
            if (schema.empty())
            {
                return Json::object();
            }

            const Json parsed = Json::parse(schema, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_object())
            {
                return Json::object();
            }
            return parsed;
        }

        /// @brief Wraps a handler's returned JSON string in an MCP tool result.
        ///
        /// A located error becomes an isError result whose text is the called tool's name
        /// followed by the error: this is the one point at which a failed call's error text
        /// is assembled, so the layer names the tool and a handler's own error carries the
        /// reason alone. A `tools/call` naming no tool frames the reason without a prefix.
        /// A success from a plain tool becomes a single text content block; a success from a
        /// content-block tool (@p returnsContentBlocks) is the `content` array itself,
        /// spliced in verbatim (falling back to a text block if it is not a valid array).
        Json MakeToolResult(string_view toolName, const Result<string>& handlerResult,
                            bool returnsContentBlocks)
        {
            if (!handlerResult)
            {
                const string text = toolName.empty()
                                        ? handlerResult.error()
                                        : fmt::format("{}: {}", toolName, handlerResult.error());
                return Json{{"content", Json::array({Json{{"type", "text"}, {"text", text}}})},
                            {"isError", true}};
            }

            if (returnsContentBlocks)
            {
                const Json content = Json::parse(*handlerResult, nullptr, false);
                if (content.is_array())
                {
                    return Json{{"content", content}, {"isError", false}};
                }
            }

            return Json{
                {"content", Json::array({Json{{"type", "text"}, {"text", *handlerResult}}})},
                {"isError", false}};
        }

        /// @brief Queues @p handler for the render thread and waits out the pump window.
        ///
        /// The pumped path both an ordinary tool call and an off-pump tool's prologue take:
        /// push a request slot, block on its condvar for RequestTimeout, and return what
        /// Pump() wrote there. On expiry the handler is *not* cancelled — the next pump still
        /// runs it — so the caller has abandoned a result that will still be produced.
        /// @param native     The server state owning the queue.
        /// @param handler    The work to run on the render thread.
        /// @param arguments  The JSON string handed to @p handler.
        /// @return The handler's result, or a located host-busy / shutting-down error.
        Result<string> RunAtPump(McpServer::Native& native,
                                 function<Result<string>(string_view)> handler, string arguments)
        {
            auto request = CreateRef<PendingRequest>();
            request->Handler = std::move(handler);
            request->Arguments = std::move(arguments);

            {
                const std::scoped_lock lock(native.QueueMutex);
                if (native.ShuttingDown)
                {
                    return std::unexpected(string("server is shutting down"));
                }
                native.Queue.push_back(request);
            }

            std::unique_lock lock(request->Mutex);
            const bool serviced =
                request->Ready.wait_for(lock, RequestTimeout, [&] { return request->Done; });
            if (!serviced)
            {
                return std::unexpected(
                    string("host busy — the render thread did not pump in time"));
            }
            return std::move(request->Value);
        }

        /// @brief Holds an off-pump slot for a scope, releasing it on every exit path.
        struct OffPumpSlot
        {
            /// @brief The server state the slot was reserved in.
            McpServer::Native* Native = nullptr;

            /// @brief The tool the slot was reserved for.
            string Tool;

            OffPumpSlot(McpServer::Native& native, string tool)
                : Native(&native), Tool(std::move(tool))
            {
            }

            OffPumpSlot(const OffPumpSlot&) = delete;
            OffPumpSlot& operator=(const OffPumpSlot&) = delete;

            ~OffPumpSlot() { Native->ReleaseOffPump(Tool); }
        };

        /// @brief Runs an off-pump tool's prologue and handler, on the calling network thread.
        ///
        /// The bounded-concurrency gate is taken first: a call over MaxOffPumpInFlight, or a
        /// second concurrent call to the same tool, is refused here rather than queued. The
        /// prologue (when declared) then runs at the pump point, and only its success reaches
        /// the handler, which runs right here and waits on nothing.
        /// @param native  The server state owning the gate and the queue.
        /// @param tool    The tool being called.
        /// @param name    The tool's registered name.
        /// @param args    The `arguments` object as a JSON string.
        /// @return The handler's result, or a located error from the gate or the prologue.
        Result<string> RunOffPump(McpServer::Native& native, const McpTool& tool,
                                  const string& name, const string& args)
        {
            {
                const std::scoped_lock lock(native.QueueMutex);
                if (native.ShuttingDown)
                {
                    return std::unexpected(string("server is shutting down"));
                }
            }

            if (!native.TryAcquireOffPump(name))
            {
                return std::unexpected(
                    string("off-pump busy — this tool is already running, or the host's two "
                           "off-pump handlers are both in flight"));
            }
            const OffPumpSlot slot(native, name);

            string snapshot;
            if (tool.PumpedPrologue)
            {
                Result<string> prologue = RunAtPump(
                    native, [&tool](string_view) { return tool.PumpedPrologue(); }, string{});
                if (!prologue)
                {
                    return std::unexpected(std::move(prologue.error()));
                }
                snapshot = std::move(*prologue);
            }

            const McpOffPumpRequest request{
                .Arguments = args,
                .Snapshot = snapshot,
                .IsCancelled = [&native]
                { return native.Cancelled.load(std::memory_order_relaxed); }};
            return tool.OffPumpHandler(request);
        }

        /// @brief Answers a single JSON-RPC message on the network thread.
        ///
        /// Protocol methods (initialize / tools/list) are answered inline with no engine
        /// access; a tools/call enqueues onto the render-thread queue and blocks on the
        /// request slot with a timeout — unless the tool declares it runs off the pump, in
        /// which case its handler runs right here (see RunOffPump). Returns the JSON-RPC
        /// response, or nullopt for a notification (no response body).
        optional<Json> DispatchMessage(McpServer::Native& native, const Json& message)
        {
            if (!message.is_object() || message.value("jsonrpc", string{}) != "2.0")
            {
                return MakeError(nullptr, ErrorInvalidRequest, "not a JSON-RPC 2.0 message");
            }

            const string method = message.value("method", string{});
            const Json id = message.contains("id") ? message.at("id") : Json(nullptr);
            const bool isNotification = !message.contains("id");

            if (method == "initialize")
            {
                Json result{
                    {"protocolVersion", ProtocolVersion},
                    {"capabilities", {{"tools", Json::object()}}},
                    {"serverInfo", {{"name", native.Info.ServerName}, {"version", ServerVersion}}}};
                return MakeResult(id, std::move(result));
            }

            if (method == "notifications/initialized")
            {
                return std::nullopt;
            }

            if (method == "tools/list")
            {
                Json tools = Json::array();
                for (const auto& [name, tool] : native.Tools)
                {
                    tools.push_back(Json{{"name", tool.Name},
                                         {"description", tool.Description},
                                         {"inputSchema", ParseInputSchema(tool.InputSchemaJson)}});
                }
                return MakeResult(id, Json{{"tools", std::move(tools)}});
            }

            if (method == "tools/call")
            {
                const Json params = message.value("params", Json::object());
                const string name = params.value("name", string{});

                const auto it = native.Tools.find(name);
                if (it == native.Tools.end())
                {
                    return MakeResult(id,
                                      MakeToolResult(name, std::unexpected(string("no such tool")),
                                                     /*returnsContentBlocks=*/false));
                }

                const McpTool& tool = it->second;
                const bool returnsContentBlocks = tool.ReturnsContentBlocks;

                const Json arguments =
                    params.contains("arguments") ? params.at("arguments") : Json::object();
                const string argsJson = arguments.dump();

                const Result<string> value = tool.RunsOffPump
                                                 ? RunOffPump(native, tool, name, argsJson)
                                                 : RunAtPump(native, tool.Handler, argsJson);
                return MakeResult(id, MakeToolResult(name, value, returnsContentBlocks));
            }

            if (isNotification)
            {
                return std::nullopt;
            }
            return MakeError(id, ErrorMethodNotFound, fmt::format("unknown method '{}'", method));
        }
    }

    Unique<McpServer> McpServer::Create(const McpServerInfo& info, const McpHost& mcpHost)
    {
        Unique<McpServer> server(new McpServer());
        server->m_Native = CreateUnique<Native>();
        Native& native = *server->m_Native;
        native.Info = info;

        native.Host = info.BindLoopbackOnly ? "127.0.0.1" : "0.0.0.0";
        const string& host = native.Host;

        // A non-empty Origin header marks a same-host browser fetch(); a real MCP client
        // sends none. Reject it before dispatch to defeat the same-host browser vector.
        native.Http.set_pre_routing_handler(
            [](const httplib::Request& req, httplib::Response& res)
            {
                if (!req.get_header_value("Origin").empty())
                {
                    res.status = 403;
                    return httplib::Server::HandlerResponse::Handled;
                }
                return httplib::Server::HandlerResponse::Unhandled;
            });

        native.Http.Post("/",
                         [&native](const httplib::Request& req, httplib::Response& res)
                         {
                             const Json message = Json::parse(req.body, nullptr, false);
                             if (message.is_discarded())
                             {
                                 const Json error =
                                     MakeError(nullptr, ErrorParse, "invalid JSON body");
                                 res.set_content(error.dump(), "application/json");
                                 return;
                             }

                             const optional<Json> response = DispatchMessage(native, message);
                             if (response)
                             {
                                 res.set_content(response->dump(), "application/json");
                             }
                             else
                             {
                                 res.status = 202;
                             }
                         });

        // Port 0 goes through bind_to_any_port, which returns the OS-chosen ephemeral
        // port a test reads back via GetPort(); a fixed port binds directly.
        if (info.Port == 0)
        {
            const int bound = native.Http.bind_to_any_port(host);
            VE_ASSERT(bound > 0, "MCP server failed to bind an ephemeral port on {}", host);
            native.BoundPort = static_cast<u16>(bound);
        }
        else
        {
            const bool bound = native.Http.bind_to_port(host, info.Port);
            VE_ASSERT(bound, "MCP server failed to bind {}:{}", host, info.Port);
            native.BoundPort = info.Port;
        }

        // The socket is bound here so GetPort() resolves immediately, but the listener
        // thread starts on the first Pump() — so tools registered between Create and the
        // first pump land before the network thread reads the (then-immutable) registry.
        RegisterWorldTools(*server, mcpHost);
        RegisterRenderTools(*server, mcpHost);
        RegisterProfileReadTools(*server, mcpHost);

        // Mutation, input-injection, and capture-control tools are opt-in: a read-only server (the
        // default) exposes none of them, so tools/list honestly reflects the server's write
        // capability. Injecting input mutates app state and a capture writes a file, so both ride the
        // same AllowMutations gate.
        if (info.AllowMutations)
        {
            RegisterMutationTools(*server, mcpHost);
            RegisterInputTools(*server, mcpHost);
            RegisterProfileWriteTools(*server, mcpHost);
        }
        return server;
    }

    void McpServer::Native::EnsureStarted()
    {
        if (Started)
        {
            return;
        }
        Started = true;

        Thread = std::thread([this] { Http.listen_after_bind(); });
        Http.wait_until_ready();
        Log::Info("MCP server listening on {}:{}", Host, BoundPort);
    }

    bool McpServer::Native::TryAcquireOffPump(const string& tool)
    {
        const std::scoped_lock lock(OffPumpMutex);
        if (OffPumpInFlight.size() >= MaxOffPumpInFlight)
        {
            return false;
        }
        if (std::ranges::find(OffPumpInFlight, tool) != OffPumpInFlight.end())
        {
            return false;
        }
        OffPumpInFlight.emplace_back(tool);
        return true;
    }

    void McpServer::Native::ReleaseOffPump(const string& tool)
    {
        const std::scoped_lock lock(OffPumpMutex);
        const auto it = std::ranges::find(OffPumpInFlight, tool);
        if (it != OffPumpInFlight.end())
        {
            OffPumpInFlight.erase(it);
        }
    }

    McpServer::McpServer() = default;

    McpServer::~McpServer()
    {
        if (!m_Native)
        {
            return;
        }
        Native& native = *m_Native;

        // An off-pump handler is in no queue, and stopping the listener joins the connection
        // pool it runs on — so signal it first and let it return of its own accord.
        native.Cancelled.store(true, std::memory_order_relaxed);

        // Reject any in-flight or newly-arriving request so its network thread unblocks,
        // then stop the listener and join.
        {
            const std::scoped_lock lock(native.QueueMutex);
            native.ShuttingDown = true;
            for (const Ref<PendingRequest>& request : native.Queue)
            {
                const std::scoped_lock slot(request->Mutex);
                request->Value = std::unexpected(string("server is shutting down"));
                request->Done = true;
                request->Ready.notify_one();
            }
            native.Queue.clear();
        }

        native.Http.stop();
        if (native.Thread.joinable())
        {
            native.Thread.join();
        }
    }

    void McpServer::RegisterTool(McpTool tool)
    {
        VE_ASSERT(m_Native, "RegisterTool on a moved-from McpServer");
        VE_ASSERT(!m_Native->Started,
                  "RegisterTool must be called before the first Pump() starts the server");

        // The threading declaration is checked here rather than at the call: a tool that
        // supplies the wrong handler for its flag would otherwise fail as a null call on
        // whichever thread it was routed to, long after the mistake.
        if (tool.RunsOffPump)
        {
            VE_ASSERT(static_cast<bool>(tool.OffPumpHandler),
                      "MCP tool '{}' declares RunsOffPump but supplies no OffPumpHandler",
                      tool.Name);
            VE_ASSERT(!tool.Handler,
                      "MCP tool '{}' declares RunsOffPump and also sets Handler — an off-pump "
                      "tool supplies OffPumpHandler alone",
                      tool.Name);
        }
        else
        {
            VE_ASSERT(static_cast<bool>(tool.Handler), "MCP tool '{}' supplies no Handler",
                      tool.Name);
            VE_ASSERT(!tool.OffPumpHandler,
                      "MCP tool '{}' sets OffPumpHandler without declaring RunsOffPump", tool.Name);
            VE_ASSERT(!tool.PumpedPrologue,
                      "MCP tool '{}' sets PumpedPrologue without declaring RunsOffPump — a "
                      "pumped tool already runs at the pump point",
                      tool.Name);
        }

        const string name = tool.Name;
        const auto [_, inserted] = m_Native->Tools.emplace(name, std::move(tool));
        VE_ASSERT(inserted, "duplicate MCP tool name '{}'", name);
    }

    u16 McpServer::GetPort() const
    {
        VE_ASSERT(m_Native, "GetPort on a moved-from McpServer");
        return m_Native->BoundPort;
    }

    void McpServer::Pump()
    {
        VE_ASSERT(m_Native, "Pump on a moved-from McpServer");

        m_Native->EnsureStarted();

        for (;;)
        {
            Ref<PendingRequest> request;
            {
                const std::scoped_lock lock(m_Native->QueueMutex);
                if (m_Native->Queue.empty())
                {
                    break;
                }
                request = std::move(m_Native->Queue.front());
                m_Native->Queue.pop_front();
            }

            Result<string> result = request->Handler(request->Arguments);
            {
                const std::scoped_lock lock(request->Mutex);
                request->Value = std::move(result);
                request->Done = true;
            }
            request->Ready.notify_one();
        }
    }
}
