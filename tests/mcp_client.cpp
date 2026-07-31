// Loopback proof for the veng::mcp client path (McpClient).
//
// Stands an McpServer up in-process on Port = 0 with a `ping` tool and an `err` tool (one
// that returns a located error, surfaced by the server as an isError tool result), drives
// a background pump loop on this (render) thread, and exercises McpClient against it:
//   - ListTools() returns the registered tools, `ping` among them.
//   - CallTool("ping", "{...}") returns a successful McpCallResult, IsError == false, the
//     echoed payload in Content.
//   - CallTool on the `err` tool returns a successful Result with IsError == true (the
//     tool-error shape the CLI later maps to a distinct exit code), its Content the tool's
//     name then the handler's reason — the server attaches the name, the handler does not.
//   - CallTool on an unknown tool returns a Result whose McpCallResult.IsError is true —
//     the server reports an unknown tool as a tools/call error result, not a protocol
//     error, so it is not a Result error here.
//   - A client pointed at a dead port returns a connection-refused Result error within the
//     timeout (not a hang).
//
// Pure logic + loopback, no GPU, so it runs in the default band. The server side reuses
// the vendored httplib compiled into veng::mcp; the client is exercised through its public
// header, which names no httplib/nlohmann type.

#include <Veng/Mcp/McpClient.h>
#include <Veng/Mcp/McpClientInfo.h>
#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpServerInfo.h>
#include <Veng/Mcp/McpTool.h>

#include <Veng/Reflection/TypeRegistry.h>

#include <nlohmann/json.hpp>

#include <algorithm>
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
}

int main()
{
    using namespace Veng;

    Mcp::McpServerInfo info;
    info.Port = 0;
    info.BindLoopbackOnly = true;

    // The client proof exercises the ping/err tools and the auto-registered world tools with
    // no world (CurrentWorld returns null), which never touch Assets. Bind Assets through a
    // never-dereferenced pointer, as mcp_loopback does — this device-free test has no Context
    // to construct one and no code path here reads it.
    TypeRegistry registry;
    AssetManager* assets = nullptr;
    const Mcp::McpHost host{.Types = registry,
                            .Assets = *assets,
                            .CurrentWorld = [] { return static_cast<Scene*>(nullptr); },
                            .Viewport = {}};

    Unique<Mcp::McpServer> server = Mcp::McpServer::Create(info, host);

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

    Mcp::McpTool err;
    err.Name = "err";
    err.Description = "Always fails with a located error.";
    err.InputSchemaJson = R"({"type":"object"})";
    err.Handler = [](string_view) -> Result<string>
    { return std::unexpected(std::string("deliberate tool failure")); };
    server->RegisterTool(std::move(err));

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
        Mcp::McpClientInfo clientInfo;
        clientInfo.Host = "127.0.0.1";
        clientInfo.Port = port;
        clientInfo.TimeoutSeconds = 10;

        Result<Unique<Mcp::McpClient>> created = Mcp::McpClient::Create(clientInfo);
        Check(created.has_value(), "Create built a client");
        if (created)
        {
            Mcp::McpClient& client = **created;

            const Result<vector<Mcp::McpToolDesc>> tools = client.ListTools();
            Check(tools.has_value(), "ListTools succeeded");
            if (tools)
            {
                const auto named = [&](string_view n)
                {
                    return std::ranges::any_of(*tools, [&](const Mcp::McpToolDesc& t)
                                               { return t.Name == n; });
                };
                Check(named("ping"), "ListTools listed the ping tool");
                Check(named("err"), "ListTools listed the err tool");
                for (const Mcp::McpToolDesc& tool : *tools)
                {
                    if (tool.Name == "ping")
                    {
                        Check(!tool.Description.empty(), "ping tool carried a description");
                        Check(!tool.InputSchema.empty(), "ping tool carried an inputSchema");
                    }
                }
            }

            const Result<Mcp::McpCallResult> okCall =
                client.CallTool("ping", R"({"message":"hi"})");
            Check(okCall.has_value(), "CallTool ping succeeded at the protocol level");
            if (okCall)
            {
                Check(!okCall->IsError, "ping was not a tool error");
                const Json payload = Json::parse(okCall->Content, nullptr, false);
                Check(payload.value("echo", std::string{}) == "hi", "ping echoed its message");
                Check(!okCall->Image.has_value(), "ping carried no image block");
            }

            const Result<Mcp::McpCallResult> errCall = client.CallTool("err", "{}");
            Check(errCall.has_value(), "CallTool err succeeded at the protocol level");
            if (errCall)
            {
                Check(errCall->IsError, "err surfaced as a tool-level error (IsError true)");
                // The server names the tool in a failed call's text; the handler supplied the
                // reason alone. This is the contract the CLI's one-label line rests on.
                Check(errCall->Content == "err: deliberate tool failure",
                      "the isError text is the tool name then the handler's reason");
            }

            const Result<Mcp::McpCallResult> unknown = client.CallTool("does.not.exist", "{}");
            Check(unknown.has_value(), "CallTool on an unknown tool is not a protocol error");
            if (unknown)
            {
                Check(unknown->IsError, "unknown tool surfaced as a tool-level error");
                Check(unknown->Content == "does.not.exist: no such tool",
                      "an unresolved name is framed by the same path");
            }
        }

        // A client pointed at a dead port must return a connection-refused Result error
        // within the timeout — not hang, not crash. Port 1 is privileged/unbound here.
        Mcp::McpClientInfo deadInfo;
        deadInfo.Host = "127.0.0.1";
        deadInfo.Port = 1;
        deadInfo.TimeoutSeconds = 2;
        Result<Unique<Mcp::McpClient>> deadClient = Mcp::McpClient::Create(deadInfo);
        Check(deadClient.has_value(), "Create built a client for the dead port");
        if (deadClient)
        {
            const Result<vector<Mcp::McpToolDesc>> deadList = (*deadClient)->ListTools();
            Check(!deadList.has_value(), "ListTools against a dead port is a Result error");
        }
    }

    done.store(true);
    pump.join();

    if (g_Failures == 0)
    {
        std::printf("mcp_client: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "mcp_client: %d check(s) failed\n", g_Failures);
    return 1;
}
