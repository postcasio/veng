// Editor MCP client-CLI conformance through the shipping veng-editor --connect path.
//
// The editor analogue of mcp_cli_conformance: it launches veng-editor --mcp against the
// hello-triangle project (an ephemeral port), reads the "listening on <ip>:<port>" line, then
// drives that running server with veng-editor --connect as a subprocess. This exercises the
// editor exe's gated --connect seam — the short-circuit interleaved with --version / --project /
// --mcp that this planset reworks — end to end through a shipped exe, which neither the game-side
// conformance (a different main) nor mcp_cli (which drives RunClientCli directly, bypassing main)
// covers.
//
// The editor opens a real window, so it needs a display as well as a device; either missing
// surfaces as "no listening line", reported as the gpu band's skip (77), the same contract as
// editor_mcp_conformance. VENG_EDITOR_BIN / VENG_EDITOR_PROJECT / VENG_EDITOR_BUILD_DIR are baked
// in by CMake; the same veng-editor exe is both the server (--mcp) and the client (--connect).

#include <nlohmann/json.hpp>

#include "support/ProcessCapture.h"

#include <cstdio>
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

    // SIGTERM the step-1 editor server on scope exit, whatever path leaves the function — so a
    // failed client-step assertion never leaks the editor process as a zombie.
    struct ServerGuard
    {
        VengTest::Launched& Launched;
        ~ServerGuard() { VengTest::Terminate(Launched); }
    };
}

int main()
{
    VengTest::Launched server;
    if (!VengTest::SpawnCaptured({VENG_EDITOR_BIN, "--project", VENG_EDITOR_PROJECT, "--build-dir",
                                  VENG_EDITOR_BUILD_DIR, "--mcp=0", "--mcp-write"},
                                 server))
    {
        std::fprintf(stderr, "editor_mcp_cli_conformance: failed to launch '%s'\n",
                     VENG_EDITOR_BIN);
        return 1;
    }

    const int port = VengTest::ReadPort(server);
    if (port == 0)
    {
        // No listening line: the editor most likely skipped for want of a Vulkan ICD or a display
        // (it opens a real window). Reap it and report the gpu-band skip code.
        VengTest::Terminate(server);
        std::fprintf(stderr, "editor_mcp_cli_conformance: no 'listening on' line from veng-editor; "
                             "treating as no device (skip)\n");
        return 77;
    }

    const ServerGuard guard{server};

    const std::string connect = "--connect=" + std::to_string(port);

    // Step 2: --list advertises both the engine tool families and the editor's own family through
    // the client. The listing is one "<name> — <description>" line per tool.
    {
        const VengTest::RunResult list =
            VengTest::RunToCompletion({VENG_EDITOR_BIN, connect, "--list"});
        Check(list.Exited, "--list client exited on its own (no force-kill)");
        Check(list.ExitCode == 0, "--list exited 0");
        Check(list.Output.find("render.stats") != std::string::npos,
              "--list advertised render.stats");
        Check(list.Output.find("editor.list_panels") != std::string::npos,
              "--list advertised editor.list_panels");
    }

    // Step 3: editor.list_panels is a read tool needing no arguments; its payload round-trips to
    // stdout as a parseable JSON object carrying the panels array.
    {
        const VengTest::RunResult panels =
            VengTest::RunToCompletion({VENG_EDITOR_BIN, connect, "editor.list_panels"});
        Check(panels.Exited, "editor.list_panels client exited on its own (no force-kill)");
        Check(panels.ExitCode == 0, "editor.list_panels exited 0");
        const Json payload = Json::parse(panels.Output, nullptr, false);
        Check(payload.is_object() && payload.contains("panels"),
              "editor.list_panels payload parsed as JSON with a panels array");
    }

    // Step 4: the error paths collapse through the editor exe's client too — a dead port to exit 2
    // and an unknown tool to exit 4 (surfaced as a tool isError), each with a human-readable line.
    {
        const VengTest::RunResult dead =
            VengTest::RunToCompletion({VENG_EDITOR_BIN, "--connect=1", "render.stats"});
        Check(dead.Exited, "dead-port client exited on its own (no force-kill)");
        Check(dead.ExitCode == 2, "a dead port exited 2 (connection refused)");
        Check(!dead.Output.empty(), "the dead-port failure wrote a human-readable line");

        const VengTest::RunResult unknown =
            VengTest::RunToCompletion({VENG_EDITOR_BIN, connect, "no.such_tool"});
        Check(unknown.Exited, "unknown-tool client exited on its own (no force-kill)");
        Check(unknown.ExitCode == 4, "an unknown tool exited 4 (tool isError)");
        Check(!unknown.Output.empty(), "the unknown-tool failure wrote a human-readable line");
    }

    if (g_Failures == 0)
    {
        std::printf("editor_mcp_cli_conformance: all checks passed (port %d)\n", port);
        return 0;
    }
    std::fprintf(stderr, "editor_mcp_cli_conformance: %d check(s) failed\n", g_Failures);
    return 1;
}
