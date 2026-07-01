// Loopback proof for the veng::mcp request path.
//
// Constructs an McpServer on Port = 0 with a `ping` tool, drives a background pump
// loop on this (render) thread, and from a client performs a real HTTP
// initialize -> tools/list (asserts `ping` present) -> tools/call ping (asserts the
// echo). It also feeds a malformed request body and asserts a clean JSON-RPC error —
// the containment check that a throw inside httplib's -fexceptions TU never unwinds
// out (no terminate). Pure logic + loopback, no GPU, so it runs in the default band.
//
// The httplib client is the same vendored header compiled here (this TU builds with
// exceptions, like the rest of the test suite), so it needs no new dependency.

#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpServerInfo.h>
#include <Veng/Mcp/McpTool.h>

#include <nlohmann/json.hpp>

#define CPPHTTPLIB_IMPLEMENTATION
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using Json = nlohmann::json;

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

    // POST a JSON-RPC message to the server and return the parsed response body.
    Json Post(httplib::Client& client, const Json& message)
    {
        const httplib::Result res = client.Post("/", message.dump(), "application/json");
        if (!res)
        {
            return Json{{"error", "no response"}};
        }
        return Json::parse(res->body, nullptr, false);
    }
}

int main()
{
    using namespace Veng;

    Mcp::McpServerInfo info;
    info.Port = 0;
    info.BindLoopbackOnly = true;

    Unique<Mcp::McpServer> server = Mcp::McpServer::Create(info);

    Mcp::McpTool ping;
    ping.Name = "ping";
    ping.Description = "Echoes its message back.";
    ping.InputSchemaJson = R"({"type":"object","properties":{"message":{"type":"string"}}})";
    ping.Handler = [](string_view argsJson) -> Result<string>
    {
        const Json args = Json::parse(argsJson, nullptr, false);
        const std::string message =
            args.is_object() ? args.value("message", std::string{}) : std::string{};
        return Json{{"echo", message}}.dump();
    };
    server->RegisterTool(std::move(ping));

    const u16 port = server->GetPort();
    Check(port != 0, "GetPort resolved an ephemeral port");

    // Drive Pump() on this thread until the client work is done — the render-thread role.
    std::atomic<bool> done{false};
    std::thread pump(
        [&]
        {
            while (!done.load())
            {
                server->Pump();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            server->Pump();
        });

    {
        httplib::Client client("127.0.0.1", port);
        client.set_connection_timeout(5, 0);
        client.set_read_timeout(10, 0);

        const Json initialize = Post(client, Json{{"jsonrpc", "2.0"},
                                                  {"id", 1},
                                                  {"method", "initialize"},
                                                  {"params", Json::object()}});
        Check(initialize.contains("result"), "initialize returned a result");
        Check(initialize["result"].value("protocolVersion", std::string{}).size() > 0,
              "initialize reported a protocolVersion");
        Check(initialize["result"]["serverInfo"].value("name", std::string{}) == "veng",
              "initialize reported the server name");

        const Json list = Post(client, Json{{"jsonrpc", "2.0"},
                                            {"id", 2},
                                            {"method", "tools/list"},
                                            {"params", Json::object()}});
        Check(list.contains("result"), "tools/list returned a result");
        bool sawPing = false;
        for (const Json& tool : list["result"]["tools"])
        {
            if (tool.value("name", std::string{}) == "ping")
            {
                sawPing = true;
            }
        }
        Check(sawPing, "tools/list listed the ping tool");

        const Json call = Post(
            client, Json{{"jsonrpc", "2.0"},
                         {"id", 3},
                         {"method", "tools/call"},
                         {"params", {{"name", "ping"}, {"arguments", {{"message", "hello"}}}}}});
        Check(call.contains("result"), "tools/call returned a result");
        Check(call["result"].value("isError", true) == false, "ping was not an error");
        const std::string text = call["result"]["content"][0].value("text", std::string{});
        const Json payload = Json::parse(text, nullptr, false);
        Check(payload.value("echo", std::string{}) == "hello", "ping echoed its message");

        // A malformed body must come back a clean error, never a terminate — the
        // containment check for the -fexceptions vendor TU.
        const httplib::Result bad = client.Post("/", std::string("{ not json"), "application/json");
        Check(static_cast<bool>(bad), "malformed request got a response");
        if (bad)
        {
            const Json error = Json::parse(bad->body, nullptr, false);
            Check(error.contains("error"), "malformed request returned a JSON-RPC error");
        }

        // An unknown tool is a tools/call error result, not a protocol error.
        const Json unknown = Post(
            client, Json{{"jsonrpc", "2.0"},
                         {"id", 4},
                         {"method", "tools/call"},
                         {"params", {{"name", "does.not.exist"}, {"arguments", Json::object()}}}});
        Check(unknown.contains("result"), "unknown tool returned a result envelope");
        Check(unknown["result"].value("isError", false) == true, "unknown tool was an error");
    }

    done.store(true);
    pump.join();

    if (g_Failures == 0)
    {
        std::printf("mcp_loopback: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "mcp_loopback: %d check(s) failed\n", g_Failures);
    return 1;
}
