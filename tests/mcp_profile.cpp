// Headless proof for the veng::mcp capture-control tools (profile.*).
//
// Constructs an McpServer with AllowMutations = true and an McpHost whose Profiler closure returns a
// live Diagnostics::Profiler, and pumps it on a background thread that also advances the profiler's
// frame and emits a scope each iteration so the ring has content. Over loopback it drives:
//   - tools/list, asserting all four verbs are present under AllowMutations.
//   - profile.stats, the read-only tool, asserting it reports a status and drop counters.
//   - profile.start + profile.stop, asserting the round-trip writes a real, complete file at the
//     returned path.
//   - profile.dump_ring, asserting it returns a path to a written file.
//   - the shape-validation error (a negative 'frames') as a whole-call isError result, not a
//     JSON-RPC protocol error.
// A second server with AllowMutations = false asserts only profile.stats is present. A third with a
// null Profiler seam asserts the verbs report the profiler unavailable. Pure logic + loopback, no
// GPU, so it runs in the default band.
//
// The runtime behaviour is proved under VE_PROFILE; under VE_PROFILE=OFF the profiler is a shell and
// the file round-trips do not apply, so those assertions are gated. The file still compiles and links
// under OFF (the OFF compile-and-link check), where the verbs return the documented disabled error.

#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpServerInfo.h>

#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Reflection/TypeRegistry.h>

#include <nlohmann/json.hpp>

#define CPPHTTPLIB_IMPLEMENTATION
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

    std::set<std::string> ToolNames(httplib::Client& client)
    {
        const Json list = Post(client, Json{{"jsonrpc", "2.0"},
                                            {"id", 1},
                                            {"method", "tools/list"},
                                            {"params", Json::object()}});
        std::set<std::string> names;
        for (const Json& tool : list["result"]["tools"])
        {
            names.insert(tool.value("name", std::string{}));
        }
        return names;
    }

    // Runs a server on a background pump thread for the duration of a scope, tearing it down cleanly.
    struct PumpedServer
    {
        std::atomic<bool> Done{false};
        std::thread Thread;

        explicit PumpedServer(Mcp::McpServer& server, Diagnostics::Profiler* profiler)
        {
            Thread = std::thread(
                [this, &server, profiler]
                {
                    while (!Done.load())
                    {
                        if (profiler != nullptr)
                        {
                            // Give the ring something to hold and advance the frame, so dump_ring has
                            // content and any frame-count capture can terminate.
                            {
                                VE_PROFILE_SCOPE("PumpScope");
                            }
                            profiler->BeginFrame();
                        }
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
    // The profile tools never touch Assets; bind a never-dereferenced AssetManager, as the other
    // headless mcp tests do.
    AssetManager* assets = nullptr;

    Diagnostics::Profiler profiler;
    profiler.SetRingEnabled(true);

    const Mcp::McpHost host{
        .Types = registry,
        .Assets = *assets,
        .Profiler = [&profiler] { return &profiler; },
    };

    Mcp::McpServerInfo info;
    info.Port = 0;
    info.AllowMutations = true;
    Unique<Mcp::McpServer> server = Mcp::McpServer::Create(info, host);
    const u16 port = server->GetPort();
    Check(port != 0, "GetPort resolved an ephemeral port");

    {
        const PumpedServer pumped(*server, &profiler);

        httplib::Client client("127.0.0.1", port);
        client.set_connection_timeout(5, 0);
        client.set_read_timeout(10, 0);

        const std::set<std::string> names = ToolNames(client);
        Check(names.count("profile.stats") == 1, "profile.stats present under AllowMutations");
        Check(names.count("profile.start") == 1, "profile.start present under AllowMutations");
        Check(names.count("profile.stop") == 1, "profile.stop present under AllowMutations");
        Check(names.count("profile.dump_ring") == 1,
              "profile.dump_ring present under AllowMutations");

        // profile.stats: read-only, always answerable.
        const Json stats = Payload(CallToolResult(client, "profile.stats", Json::object()));
        Check(stats.is_object() && stats.contains("status"), "profile.stats reports a status");
        Check(stats.contains("dropped_events"), "profile.stats reports drop counters");

        // profile.start + profile.stop round-trip to a real file.
        const Json started = CallToolResult(client, "profile.start", Json{{"name", "mcp-capture"}});
        Check(!IsError(started), "profile.start succeeded");
        Check(Payload(started).value("status", std::string{}) == "capturing",
              "profile.start reports the capturing state");

        const Json stopped = CallToolResult(client, "profile.stop", Json::object());
        Check(!IsError(stopped), "profile.stop succeeded");
        const std::string stopPath = Payload(stopped).value("path", std::string{});
        Check(!stopPath.empty(), "profile.stop returned a path");
#if defined(VE_PROFILE) && VE_PROFILE
        Check(std::filesystem::exists(stopPath), "the profile.stop path names a real file");
        {
            std::ifstream in(stopPath, std::ios::binary);
            char magic[8] = {};
            in.read(magic, sizeof(magic));
            Check(std::string(magic, 8) == "VENGTRAC", "the written capture carries the magic");
        }
#endif

        // profile.dump_ring returns a path to a written file.
        const Json dumped = CallToolResult(client, "profile.dump_ring", Json{{"name", "mcp-ring"}});
        Check(!IsError(dumped), "profile.dump_ring succeeded");
        const std::string dumpPath = Payload(dumped).value("path", std::string{});
        Check(!dumpPath.empty(), "profile.dump_ring returned a path");
#if defined(VE_PROFILE) && VE_PROFILE
        Check(std::filesystem::exists(dumpPath), "the profile.dump_ring path names a real file");
#endif

        // A shape error surfaces as an isError tool result, not a JSON-RPC protocol error.
        const Json bad = CallToolResult(client, "profile.start", Json{{"frames", -1}});
        Check(IsError(bad), "a negative 'frames' is a whole-call tool error");
    }

    // A read-only server exposes only the read-only profile tool.
    {
        Mcp::McpServerInfo readInfo;
        readInfo.Port = 0;
        readInfo.AllowMutations = false;
        Unique<Mcp::McpServer> readServer = Mcp::McpServer::Create(readInfo, host);
        const u16 readPort = readServer->GetPort();

        const PumpedServer pumped(*readServer, nullptr);
        httplib::Client client("127.0.0.1", readPort);
        client.set_connection_timeout(5, 0);

        const std::set<std::string> names = ToolNames(client);
        Check(names.count("profile.stats") == 1, "profile.stats present on a read-only server");
        Check(names.count("profile.start") == 0, "profile.start absent on a read-only server");
        Check(names.count("profile.stop") == 0, "profile.stop absent on a read-only server");
        Check(names.count("profile.dump_ring") == 0,
              "profile.dump_ring absent on a read-only server");
    }

    // A host with a null Profiler seam reports the profiler unavailable rather than dereferencing it.
    {
        const Mcp::McpHost noProfiler{.Types = registry, .Assets = *assets};
        Mcp::McpServerInfo nullInfo;
        nullInfo.Port = 0;
        nullInfo.AllowMutations = true;
        Unique<Mcp::McpServer> nullServer = Mcp::McpServer::Create(nullInfo, noProfiler);
        const u16 nullPort = nullServer->GetPort();

        const PumpedServer pumped(*nullServer, nullptr);
        httplib::Client client("127.0.0.1", nullPort);
        client.set_connection_timeout(5, 0);
        client.set_read_timeout(10, 0);

        Check(IsError(CallToolResult(client, "profile.stats", Json::object())),
              "profile.stats reports unavailable with a null Profiler seam");
        Check(IsError(CallToolResult(client, "profile.start", Json::object())),
              "profile.start reports unavailable with a null Profiler seam");
    }

    if (g_Failures == 0)
    {
        std::printf("mcp_profile: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "mcp_profile: %d check(s) failed\n", g_Failures);
    return 1;
}
