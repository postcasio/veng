// Headless proof for the veng::mcp video-capture tools (render.capture_*).
//
// Constructs an McpServer over an McpHost with no recorder — which is the honest headless shape: a
// VideoRecorder needs a render context, and a run with no swap chain presents no frame to record.
// So what this pins is everything about the tools that does not need an encoder, which is all of
// their contract bar the recording itself:
//   - the write gate: all three verbs present under AllowMutations, only render.capture_status on a
//     read-only server, so tools/list stays truthful about the server's write capability.
//   - render.capture_status answering rather than erroring where nothing can record: available
//     false, status Off, every state field present, and the reason stated.
//   - render.capture_start refusing with that same reason instead of dereferencing the seam.
//   - the handler's own settings validation, which is the only validation there is (the server
//     echoes InputSchemaJson into tools/list and checks nothing against it): a zero frame rate, an
//     unknown codec name, and an unknown key — a directory among them, since a capture is named and
//     never pathed — are each a whole-call tool error.
// Pure logic + loopback, no GPU, so it runs in the default band.

#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpServerInfo.h>

#include <Veng/Reflection/TypeRegistry.h>

#include <nlohmann/json.hpp>

#define CPPHTTPLIB_IMPLEMENTATION
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <set>
#include <string>
#include <thread>

using Json = nlohmann::json;
using namespace Veng;

namespace
{
    int g_Failures = 0;

    void Check(bool condition, const char* what)
    {
        if (!condition)
        {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++g_Failures;
        }
    }

    Json Post(httplib::Client& client, const Json& message)
    {
        const httplib::Result res = client.Post("/", message.dump(), "application/json");
        if (!res)
        {
            return Json{{"error", "no response"}};
        }
        return Json::parse(res->body, nullptr, false);
    }

    int g_Id = 100;

    Json CallToolResult(httplib::Client& client, const std::string& name, const Json& args)
    {
        const Json response = Post(client, Json{{"jsonrpc", "2.0"},
                                                {"id", g_Id++},
                                                {"method", "tools/call"},
                                                {"params", {{"name", name}, {"arguments", args}}}});
        return response.contains("result") ? response["result"] : Json(nullptr);
    }

    Json Payload(const Json& result)
    {
        if (!result.is_object() || !result.contains("content"))
        {
            return Json(nullptr);
        }
        return Json::parse(result["content"][0].value("text", std::string{}), nullptr, false);
    }

    bool IsError(const Json& result)
    {
        return result.is_object() && result.value("isError", false);
    }

    Json ToolList(httplib::Client& client)
    {
        const Json list = Post(client, Json{{"jsonrpc", "2.0"},
                                            {"id", 1},
                                            {"method", "tools/list"},
                                            {"params", Json::object()}});
        return list["result"]["tools"];
    }

    std::set<std::string> ToolNames(httplib::Client& client)
    {
        std::set<std::string> names;
        for (const Json& tool : ToolList(client))
        {
            names.insert(tool.value("name", std::string{}));
        }
        return names;
    }

    Json SchemaOf(httplib::Client& client, const std::string& name)
    {
        for (const Json& tool : ToolList(client))
        {
            if (tool.value("name", std::string{}) == name)
            {
                return tool.value("inputSchema", Json::object());
            }
        }
        return Json(nullptr);
    }

    // Runs a server on a background pump thread for the duration of a scope, tearing it down cleanly.
    struct PumpedServer
    {
        std::atomic<bool> Done{false};
        std::thread Thread;

        explicit PumpedServer(Mcp::McpServer& server)
        {
            Thread = std::thread(
                [this, &server]
                {
                    while (!Done.load())
                    {
                        server.Pump();
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                    server.Pump();
                });
        }

        ~PumpedServer()
        {
            Done.store(true);
            Thread.join();
        }
    };
}

int main()
{
    TypeRegistry registry;
    // The capture tools never touch Assets; bind a never-dereferenced AssetManager, as the other
    // headless mcp tests do.
    AssetManager* assets = nullptr;

    const Mcp::McpHost host{.Types = registry, .Assets = *assets};

    Mcp::McpServerInfo info;
    info.Port = 0;
    info.AllowMutations = true;
    Unique<Mcp::McpServer> server = Mcp::McpServer::Create(info, host);
    const u16 port = server->GetPort();
    Check(port != 0, "GetPort resolved an ephemeral port");

    {
        const PumpedServer pumped(*server);

        httplib::Client client("127.0.0.1", port);
        client.set_connection_timeout(5, 0);
        client.set_read_timeout(10, 0);

        const std::set<std::string> names = ToolNames(client);
        Check(names.count("render.capture_status") == 1,
              "render.capture_status present under AllowMutations");
        Check(names.count("render.capture_start") == 1,
              "render.capture_start present under AllowMutations");
        Check(names.count("render.capture_stop") == 1,
              "render.capture_stop present under AllowMutations");

        // The start verb's advertised schema names the settings and no directory: a capture is
        // named, and the engine places the file.
        {
            const Json schema = SchemaOf(client, "render.capture_start");
            const Json properties =
                schema.is_object() ? schema.value("properties", Json::object()) : Json::object();
            Check(properties.contains("lockstep") && properties.contains("name") &&
                      properties.contains("include_overlay"),
                  "render.capture_start advertises its settings");
            Check(!properties.contains("directory") && !properties.contains("Directory"),
                  "render.capture_start advertises no directory");
        }

        // The read-only verb answers where nothing can record, rather than erroring: a caller polls
        // it to learn a capture has finished, so it has to be answerable in every state.
        const Json status = CallToolResult(client, "render.capture_status", Json::object());
        Check(!IsError(status), "render.capture_status answered");
        const Json state = Payload(status);
        Check(state.is_object() && state.value("status", std::string{}) == "Off",
              "render.capture_status reports Off");
        Check(state.is_object() && !state.value("available", true),
              "render.capture_status reports the run cannot record");
        Check(state.is_object() && !state.value("reason", std::string{}).empty(),
              "render.capture_status states why");

        for (const char* field :
             {"status", "available", "lockstep", "encoding", "include_overlay", "codec",
              "bitrate_mbps", "extent", "frames_acquired", "frames_appended", "frame_budget",
              "duration_seconds", "frames_waited", "waited_for_encoder_ms", "audio_blocks",
              "audio_overruns", "bytes_written", "path", "last_error"})
        {
            Check(state.is_object() && state.contains(field),
                  "render.capture_status reports every state field");
        }

        // A start with no reachable recorder refuses with the reason rather than dereferencing it.
        Check(IsError(CallToolResult(client, "render.capture_start", Json::object())),
              "render.capture_start reports the capture unavailable");
        Check(IsError(CallToolResult(client, "render.capture_stop", Json::object())),
              "render.capture_stop reports the capture unavailable");

        // The handler owns settings validation — the server validates nothing against the schema.
        Check(IsError(CallToolResult(client, "render.capture_start", Json{{"frame_rate", 0}})),
              "a zero frame rate is a whole-call tool error");
        Check(IsError(CallToolResult(client, "render.capture_start", Json{{"codec", "AV1"}})),
              "an unknown codec is a whole-call tool error");
        Check(IsError(CallToolResult(client, "render.capture_start",
                                     Json{{"Directory", "/tmp/captures"}})),
              "a directory field is rejected as unknown");
        Check(IsError(CallToolResult(client, "render.capture_start",
                                     Json{{"codec", "H264"}, {"encoding", "Hdr10"}})),
              "a codec that cannot carry the encoding is a whole-call tool error");
    }

    // A read-only server exposes only the read-only capture tool.
    {
        Mcp::McpServerInfo readInfo;
        readInfo.Port = 0;
        readInfo.AllowMutations = false;
        Unique<Mcp::McpServer> readServer = Mcp::McpServer::Create(readInfo, host);
        const u16 readPort = readServer->GetPort();

        const PumpedServer pumped(*readServer);
        httplib::Client client("127.0.0.1", readPort);
        client.set_connection_timeout(5, 0);

        const std::set<std::string> names = ToolNames(client);
        Check(names.count("render.capture_status") == 1,
              "render.capture_status present on a read-only server");
        Check(names.count("render.capture_start") == 0,
              "render.capture_start absent on a read-only server");
        Check(names.count("render.capture_stop") == 0,
              "render.capture_stop absent on a read-only server");
    }

    if (g_Failures == 0)
    {
        std::printf("mcp_capture: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "mcp_capture: %d check(s) failed\n", g_Failures);
    return 1;
}
