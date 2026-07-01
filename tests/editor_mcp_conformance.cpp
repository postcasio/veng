// Editor MCP conformance smoke through the shipping veng-editor path.
//
// Launches veng-editor against the hello-triangle project with --mcp=0 (an ephemeral port) +
// --mcp-write, reads its stdout for the "listening on <ip>:<port>" readiness line, then drives a
// real HTTP exchange over loopback: initialize, tools/list, and — against the startup level
// document the editor opens — render.stats, editor.set_field on the level's renderSettings
// (a Bloom topology toggle, forcing the viewport's Configure recompile), a panel screenshot
// through the recompiled output, and render.stats again. Each call asserts a non-error result
// and, decisively, that the editor process is still alive afterwards: render.stats and the
// set-field-then-recompile sequence are the two calls that crashed a live editor session when
// the MCP host captured engine references before Run() created them.
//
// The editor opens a real window, so the test needs a display as well as a device; either
// missing surfaces as "no listening line", reported as the gpu band's skip (77), the same
// contract as mcp_conformance. VENG_EDITOR_BIN / VENG_EDITOR_PROJECT / VENG_EDITOR_BUILD_DIR are
// baked in by CMake. The spawn/read-port/terminate scaffolding is the shared
// support/ProcessCapture.h.

#include <nlohmann/json.hpp>

#define CPPHTTPLIB_IMPLEMENTATION
#include <httplib.h>

#include "support/ProcessCapture.h"

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

    // POST a JSON-RPC message and return the parsed response body.
    Json Post(httplib::Client& client, const Json& message)
    {
        const httplib::Result res = client.Post("/", message.dump(), "application/json");
        if (!res)
        {
            return Json{{"error", "no response"}};
        }
        return Json::parse(res->body, nullptr, false);
    }

    // Calls a tool and returns the parsed JSON-RPC response.
    Json CallTool(httplib::Client& client, int id, const std::string& name, const Json& arguments)
    {
        return Post(client, Json{{"jsonrpc", "2.0"},
                                 {"id", id},
                                 {"method", "tools/call"},
                                 {"params", {{"name", name}, {"arguments", arguments}}}});
    }

    // A tool call's inner JSON payload (the first text content block), or a discarded parse.
    Json ToolPayload(const Json& response)
    {
        if (!response.contains("result") || response["result"]["content"].empty())
        {
            return Json::value_t::discarded;
        }
        const std::string text = response["result"]["content"][0].value("text", std::string{});
        return Json::parse(text, nullptr, false);
    }

    // Finds the open panel carrying the named inspectable (the startup level document), retrying
    // while the editor is still adopting its startup document in its first frames.
    std::string FindPanelWithInspectable(httplib::Client& client, const std::string& inspectable)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < deadline)
        {
            const Json response = CallTool(client, 100, "editor.list_panels", Json::object());
            const Json payload = ToolPayload(response);
            if (payload.is_object() && payload.contains("panels"))
            {
                for (const Json& panel : payload["panels"])
                {
                    for (const Json& name : panel.value("inspectables", Json::array()))
                    {
                        if (name == inspectable)
                        {
                            return panel.value("title", std::string{});
                        }
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        return {};
    }
}

int main()
{
    VengTest::Launched launched;
    if (!VengTest::SpawnCaptured({VENG_EDITOR_BIN, "--project", VENG_EDITOR_PROJECT, "--build-dir",
                                  VENG_EDITOR_BUILD_DIR, "--mcp=0", "--mcp-write"},
                                 launched))
    {
        std::fprintf(stderr, "editor_mcp_conformance: failed to launch '%s'\n", VENG_EDITOR_BIN);
        return 1;
    }

    const int port = VengTest::ReadPort(launched);
    if (port == 0)
    {
        // No listening line: the editor most likely skipped for want of a Vulkan ICD or a display
        // (it opens a real window). Reap it and report the gpu-band skip code.
        VengTest::Terminate(launched);
        std::fprintf(stderr, "editor_mcp_conformance: no 'listening on' line from veng-editor; "
                             "treating as no device (skip)\n");
        return 77;
    }

    {
        httplib::Client client("127.0.0.1", port);
        client.set_connection_timeout(5, 0);
        client.set_read_timeout(15, 0);

        const Json initialize = Post(client, Json{{"jsonrpc", "2.0"},
                                                  {"id", 1},
                                                  {"method", "initialize"},
                                                  {"params", Json::object()}});
        Check(initialize.contains("result"), "initialize returned a result");
        Check(initialize["result"]["serverInfo"].value("name", std::string{}) == "veng-editor",
              "initialize reported the editor's server name");

        // The editor tool family registers beside the engine tools.
        const Json list = Post(client, Json{{"jsonrpc", "2.0"},
                                            {"id", 2},
                                            {"method", "tools/list"},
                                            {"params", Json::object()}});
        bool sawListPanels = false;
        bool sawSetField = false;
        bool sawScreenshotPanel = false;
        bool sawRenderStats = false;
        for (const Json& tool : list.contains("result") ? list["result"]["tools"] : Json::array())
        {
            const std::string name = tool.value("name", std::string{});
            sawListPanels = sawListPanels || name == "editor.list_panels";
            sawSetField = sawSetField || name == "editor.set_field";
            sawScreenshotPanel = sawScreenshotPanel || name == "editor.screenshot_panel";
            sawRenderStats = sawRenderStats || name == "render.stats";
        }
        Check(sawListPanels, "tools/list included editor.list_panels");
        Check(sawSetField, "tools/list included editor.set_field");
        Check(sawScreenshotPanel, "tools/list included editor.screenshot_panel");
        Check(sawRenderStats, "tools/list included render.stats");

        // The startup level document is the panel exposing renderSettings; its title is also the
        // viewport name the render tools resolve (GetSceneViewportNames reports document titles).
        const std::string panel = FindPanelWithInspectable(client, "renderSettings");
        Check(!panel.empty(), "the startup level document exposes renderSettings");

        if (!panel.empty())
        {
            // render.stats reaches through McpHost::Assets into the render context — the call
            // that crashed when the host captured GetAssetManager() before Run() created it.
            const Json stats = CallTool(client, 3, "render.stats", Json{{"viewport", panel}});
            Check(stats.contains("result") && stats["result"].value("isError", true) == false,
                  "render.stats succeeded against the level document viewport");
            const Json statsPayload = ToolPayload(stats);
            Check(statsPayload.is_object() && statsPayload.contains("visible") &&
                      statsPayload.contains("gpu_frame_time_ms"),
                  "render.stats reported the cull funnel and GPU frame time");
            Check(VengTest::IsRunning(launched), "the editor survived render.stats");

            // A Bloom toggle is a topology change: the panel's OnInspectableChanged pushes it to
            // the viewport, whose next OnUI runs the SceneRenderer Configure recompile.
            const Json setField = CallTool(client, 4, "editor.set_field",
                                           Json{{"panel", panel},
                                                {"inspectable", "renderSettings"},
                                                {"values", Json{{"Bloom", false}}}});
            Check(setField.contains("result") && setField["result"].value("isError", true) == false,
                  "editor.set_field applied Bloom=false to renderSettings");

            // The screenshot's Download runs against the freshly recompiled output, and the second
            // stats call lands frames after the recompile — together they pin the editor riding
            // out a mid-session Configure driven from the MCP pump.
            const Json shot =
                CallTool(client, 5, "editor.screenshot_panel", Json{{"panel", panel}});
            Check(shot.contains("result") && shot["result"].value("isError", true) == false,
                  "editor.screenshot_panel captured the recompiled viewport");

            const Json statsAfter = CallTool(client, 6, "render.stats", Json{{"viewport", panel}});
            Check(statsAfter.contains("result") &&
                      statsAfter["result"].value("isError", true) == false,
                  "render.stats succeeded after the Bloom recompile");
            Check(VengTest::IsRunning(launched),
                  "the editor survived the Bloom toggle + screenshot sequence");
        }
    }

    VengTest::Terminate(launched);

    if (g_Failures == 0)
    {
        std::printf("editor_mcp_conformance: all checks passed (port %d)\n", port);
        return 0;
    }
    std::fprintf(stderr, "editor_mcp_conformance: %d check(s) failed\n", g_Failures);
    return 1;
}
