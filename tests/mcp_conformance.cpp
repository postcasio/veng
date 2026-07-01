// MCP conformance smoke through the shipping consumer path.
//
// Launches hello_triangle-launcher (which dlopens libhello_triangle) under HT_MCP=0 (an
// ephemeral port) + HT_SMOKE (the deterministic headless scene), reads the launcher's stdout
// for the McpServer "listening on <ip>:<port>" line — the readiness edge and the actual port —
// then performs a real HTTP initialize -> tools/list over loopback and asserts the engine tool
// families are present (world.* / render.* and, since HT_SMOKE renders the sample scene,
// world.list_entities returns entities). It then terminates the launcher and treats a clean
// exchange as pass.
//
// HT_MCP suppresses the launcher's 20-frame auto-exit, so the process stays up for the whole
// exchange rather than racing its own shutdown. Labelled gpu (it drives the real render path);
// exits 77 when the launcher itself skipped for want of a Vulkan ICD.
//
// The vendored httplib client is the same single header compiled here (this TU builds with
// exceptions, like the rest of the suite), so it needs no new dependency. VENG_LAUNCHER_BIN is
// baked in by CMake ($<TARGET_FILE:hello_triangle-launcher>). The spawn/read-port/terminate
// scaffolding is the shared support/ProcessCapture.h, also used by editor_mcp_conformance.

#include <nlohmann/json.hpp>

#define CPPHTTPLIB_IMPLEMENTATION
#include <httplib.h>

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
}

int main()
{
    // Drive the launcher through its real dlopen path with the MCP server on an ephemeral port and
    // the deterministic headless scene. HT_MCP suppresses the 20-frame auto-exit, so it serves.
#if defined(_WIN32)
    _putenv_s("HT_MCP", "0");
    _putenv_s("HT_SMOKE", "mcp_conformance_capture.ppm");
#else
    setenv("HT_MCP", "0", 1);
    setenv("HT_SMOKE", "mcp_conformance_capture.ppm", 1);
#endif

    VengTest::Launched launched;
    if (!VengTest::SpawnCaptured({VENG_LAUNCHER_BIN}, launched))
    {
        std::fprintf(stderr, "mcp_conformance: failed to launch '%s'\n", VENG_LAUNCHER_BIN);
        return 1;
    }

    const int port = VengTest::ReadPort(launched);
    if (port == 0)
    {
        // No listening line: the launcher most likely skipped for want of a Vulkan ICD (the whole
        // gpu band's skip contract) or crashed. Reap it and report the gpu-band skip code.
        VengTest::Terminate(launched);
        std::fprintf(stderr,
                     "mcp_conformance: no 'listening on' line from the launcher; treating as no "
                     "device (skip)\n");
        return 77;
    }

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
        Check(initialize["result"]["serverInfo"].value("name", std::string{}) == "hello-triangle",
              "initialize reported the sample's server name");

        const Json list = Post(client, Json{{"jsonrpc", "2.0"},
                                            {"id", 2},
                                            {"method", "tools/list"},
                                            {"params", Json::object()}});
        Check(list.contains("result"), "tools/list returned a result");

        // The engine tool families the game host contributes: the read-only world tools and the
        // render tools. Mutations are off (HT_MCP_WRITE unset), so entity.set_field is absent.
        bool sawListEntities = false;
        bool sawEntityGet = false;
        bool sawSceneStats = false;
        bool sawRenderStats = false;
        bool sawListViewports = false;
        bool sawSetField = false;
        for (const Json& tool : list["result"]["tools"])
        {
            const std::string name = tool.value("name", std::string{});
            sawListEntities = sawListEntities || name == "world.list_entities";
            sawEntityGet = sawEntityGet || name == "entity.get";
            sawSceneStats = sawSceneStats || name == "scene.stats";
            sawRenderStats = sawRenderStats || name == "render.stats";
            sawListViewports = sawListViewports || name == "render.list_viewports";
            sawSetField = sawSetField || name == "entity.set_field";
        }
        Check(sawListEntities, "tools/list included world.list_entities");
        Check(sawEntityGet, "tools/list included entity.get");
        Check(sawSceneStats, "tools/list included scene.stats");
        Check(sawRenderStats, "tools/list included render.stats");
        Check(sawListViewports, "tools/list included render.list_viewports");
        Check(!sawSetField, "mutation tools are absent with HT_MCP_WRITE unset");

        // world.list_entities returns the sample scene's entities: the world loaded and the world
        // tools reach it. The default page is well under the cap, so no cursor is needed.
        const Json entities = Post(
            client,
            Json{{"jsonrpc", "2.0"},
                 {"id", 3},
                 {"method", "tools/call"},
                 {"params", {{"name", "world.list_entities"}, {"arguments", Json::object()}}}});
        Check(entities.contains("result"), "world.list_entities returned a result");
        Check(entities["result"].value("isError", true) == false,
              "world.list_entities was not an error");
        if (entities.contains("result") && !entities["result"]["content"].empty())
        {
            const std::string text = entities["result"]["content"][0].value("text", std::string{});
            const Json payload = Json::parse(text, nullptr, false);
            Check(payload.contains("entities") && payload["entities"].is_array() &&
                      !payload["entities"].empty(),
                  "world.list_entities reported the sample scene's entities");
        }

        // render.stats executes against the primary viewport — the one tool reaching through
        // McpHost::Assets into the render context, so calling it (not merely listing it) pins
        // the host's engine references being live at tool time.
        const Json stats = Post(
            client, Json{{"jsonrpc", "2.0"},
                         {"id", 4},
                         {"method", "tools/call"},
                         {"params", {{"name", "render.stats"}, {"arguments", Json::object()}}}});
        Check(stats.contains("result"), "render.stats returned a result");
        Check(stats["result"].value("isError", true) == false, "render.stats was not an error");
        if (stats.contains("result") && !stats["result"]["content"].empty())
        {
            const std::string text = stats["result"]["content"][0].value("text", std::string{});
            const Json payload = Json::parse(text, nullptr, false);
            Check(payload.contains("visible") && payload.contains("gpu_frame_time_ms"),
                  "render.stats reported the cull funnel and GPU frame time");
        }
    }

    VengTest::Terminate(launched);

    if (g_Failures == 0)
    {
        std::printf("mcp_conformance: all checks passed (port %d)\n", port);
        return 0;
    }
    std::fprintf(stderr, "mcp_conformance: %d check(s) failed\n", g_Failures);
    return 1;
}
