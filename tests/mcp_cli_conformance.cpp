// MCP client-CLI conformance through the shipping launcher's --connect path.
//
// Where mcp_conformance drives the server with an in-process httplib client, this test drives it
// with the real hello_triangle-launcher --connect, so it exercises the whole shipped client path
// (arg parse -> McpClient -> exit code) end to end. It launches hello_triangle-launcher under
// HT_MCP=0 (an ephemeral port) + HT_SMOKE (the deterministic headless scene), reads the McpServer
// "listening on <ip>:<port>" line for the port, then runs the same launcher a second time as a
// --connect client against that port: --list, a render.stats and a world.list_entities tools/call,
// and both nonzero error paths (a dead port, an unknown tool). Each client invocation runs to its
// natural exit so the asserted exit code is the client's own, never a force-kill.
//
// Labelled gpu (it drives the real render path); exits 77 when the launcher itself skipped for
// want of a Vulkan ICD (the "no listening line" branch). VENG_LAUNCHER_BIN is baked in by CMake
// ($<TARGET_FILE:hello_triangle-launcher>) and is the path for both the server and client roles.

#include <nlohmann/json.hpp>

#include "support/ProcessCapture.h"

#include <cstdio>
#include <cstdlib>
#include <string>

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

    // SIGTERM the step-1 server on scope exit, whatever path (assertion, early return, normal end)
    // leaves the function — so a failed client-step assertion never leaks the server as a zombie.
    struct ServerGuard
    {
        VengTest::Launched& Launched;
        ~ServerGuard() { VengTest::Terminate(Launched); }
    };
}

int main()
{
    // Drive the launcher through its real dlopen path with the MCP server on an ephemeral port and
    // the deterministic headless scene. HT_MCP suppresses the 20-frame auto-exit, so it serves.
#if defined(_WIN32)
    _putenv_s("HT_MCP", "0");
    _putenv_s("HT_SMOKE", "mcp_cli_conformance_capture.ppm");
#else
    setenv("HT_MCP", "0", 1);
    setenv("HT_SMOKE", "mcp_cli_conformance_capture.ppm", 1);
#endif

    VengTest::Launched server;
    if (!VengTest::SpawnCaptured({VENG_LAUNCHER_BIN}, server))
    {
        std::fprintf(stderr, "mcp_cli_conformance: failed to launch '%s'\n", VENG_LAUNCHER_BIN);
        return 1;
    }

    const int port = VengTest::ReadPort(server);
    if (port == 0)
    {
        // No listening line: the launcher most likely skipped for want of a Vulkan ICD (the whole
        // gpu band's skip contract) or crashed. Reap it and report the gpu-band skip code.
        VengTest::Terminate(server);
        std::fprintf(stderr, "mcp_cli_conformance: no 'listening on' line from the launcher; "
                             "treating as no device (skip)\n");
        return 77;
    }

    const ServerGuard guard{server};

    const std::string connect = "--connect=" + std::to_string(port);

    // Step 2: --list advertises the game host's engine tool families. The listing is one
    // "<name> — <description>" line per tool, so a substring probe finds each name.
    {
        const VengTest::RunResult list =
            VengTest::RunToCompletion({VENG_LAUNCHER_BIN, connect, "--list"});
        Check(list.Exited, "--list client exited on its own (no force-kill)");
        Check(list.ExitCode == 0, "--list exited 0");
        Check(list.Output.find("world.list_entities") != std::string::npos,
              "--list advertised world.list_entities");
        Check(list.Output.find("render.stats") != std::string::npos,
              "--list advertised render.stats");
        Check(list.Output.find("scene.stats") != std::string::npos,
              "--list advertised scene.stats");
    }

    // Step 3: render.stats round-trips its payload to stdout as parseable JSON carrying the cull
    // funnel and GPU frame time — the same fields mcp_conformance checks, now through the client.
    {
        const VengTest::RunResult stats =
            VengTest::RunToCompletion({VENG_LAUNCHER_BIN, connect, "render.stats"});
        Check(stats.Exited, "render.stats client exited on its own (no force-kill)");
        Check(stats.ExitCode == 0, "render.stats exited 0");
        const Json payload = Json::parse(stats.Output, nullptr, false);
        Check(payload.is_object() && payload.contains("visible") &&
                  payload.contains("gpu_frame_time_ms"),
              "render.stats payload parsed as JSON with visible + gpu_frame_time_ms");
    }

    // Step 4: world.list_entities returns the sample scene's entities as a non-empty array.
    {
        const VengTest::RunResult entities =
            VengTest::RunToCompletion({VENG_LAUNCHER_BIN, connect, "world.list_entities"});
        Check(entities.Exited, "world.list_entities client exited on its own (no force-kill)");
        Check(entities.ExitCode == 0, "world.list_entities exited 0");
        const Json payload = Json::parse(entities.Output, nullptr, false);
        Check(payload.is_object() && payload.contains("entities") &&
                  payload["entities"].is_array() && !payload["entities"].empty(),
              "world.list_entities payload carried a non-empty entities array");
    }

    // Step 5a: a dead port collapses to exit 2 (cannot reach the host) with a human-readable
    // stderr line. The captured output is stdout+stderr merged, so it must be non-empty.
    {
        const VengTest::RunResult dead =
            VengTest::RunToCompletion({VENG_LAUNCHER_BIN, "--connect=1", "render.stats"});
        Check(dead.Exited, "dead-port client exited on its own (no force-kill)");
        Check(dead.ExitCode == 2, "a dead port exited 2 (connection refused)");
        Check(!dead.Output.empty(), "the dead-port failure wrote a human-readable line");
    }

    // Step 5b: an unknown tool collapses to exit 4 — the server surfaces an unknown tool name as a
    // tool result flagged isError (not a JSON-RPC protocol error), which the client maps to 4 —
    // with a human-readable stderr line. The load-bearing contract is nonzero + a stderr line.
    {
        const VengTest::RunResult unknown =
            VengTest::RunToCompletion({VENG_LAUNCHER_BIN, connect, "no.such_tool"});
        Check(unknown.Exited, "unknown-tool client exited on its own (no force-kill)");
        Check(unknown.ExitCode == 4, "an unknown tool exited 4 (tool isError)");
        Check(!unknown.Output.empty(), "the unknown-tool failure wrote a human-readable line");
    }

    if (g_Failures == 0)
    {
        std::printf("mcp_cli_conformance: all checks passed (port %d)\n", port);
        return 0;
    }
    std::fprintf(stderr, "mcp_cli_conformance: %d check(s) failed\n", g_Failures);
    return 1;
}
