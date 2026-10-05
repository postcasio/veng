// Headless proof for the read-only haptics tool (haptics.state).
//
// Stands an McpServer up (read-only, the default) over an McpHost whose Haptics closure resolves a
// HapticsEngine with no device, plays a clip on one pad slot and an application-owned clip on a seat
// with no pad, mixes one frame, and reads haptics.state over loopback: the playing pad is reported
// with its mixed levels, both instances appear with their targets and the pad each resolved to, and
// idle slots are left out. A second server whose host leaves Haptics null reports the tool
// unavailable. The engine is mutated only before the pump starts, so the pump thread only reads it.
// Pure logic + loopback, no GPU, so it runs in the default band.

#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpServerInfo.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Haptics/Haptics.h>
#include <Veng/Reflection/TypeRegistry.h>

#include <nlohmann/json.hpp>

#define CPPHTTPLIB_IMPLEMENTATION
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using Json = nlohmann::json;
using namespace Veng;

namespace
{
    int g_Failures = 0;

    void Check(const bool condition, const char* what)
    {
        if (!condition)
        {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++g_Failures;
        }
    }

    // Calls a tool and returns its result object (null on a transport failure).
    Json CallTool(httplib::Client& client, const std::string& name)
    {
        const Json request{{"jsonrpc", "2.0"},
                           {"id", 7},
                           {"method", "tools/call"},
                           {"params", {{"name", name}, {"arguments", Json::object()}}}};
        const httplib::Result res = client.Post("/", request.dump(), "application/json");
        if (!res)
        {
            return nullptr;
        }
        const Json response = Json::parse(res->body, nullptr, false);
        return response.contains("result") ? response["result"] : Json(nullptr);
    }

    // Runs @p server's pump on a thread while @p body talks to it, then stops the pump.
    template <class Body>
    void Serve(Mcp::McpServer& server, Body&& body)
    {
        std::atomic<bool> done{false};
        std::thread pump(
            [&]
            {
                while (!done.load())
                {
                    server.Pump();
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                server.Pump();
            });
        {
            httplib::Client client("127.0.0.1", server.GetPort());
            client.set_connection_timeout(5, 0);
            client.set_read_timeout(10, 0);
            body(client);
        }
        done.store(true);
        pump.join();
    }
}

int main()
{
    TypeRegistry registry;
    // The haptics tool never touches Assets; bind a never-dereferenced AssetManager, as the other
    // headless mcp tests do.
    AssetManager* assets = nullptr;

    const Curve1D flat{.Keys = {{.Time = 0.0f, .Value = 0.5f}}};
    const AssetHandle<Haptics::RumbleClip> clip = AssetManager::Adopt(Haptics::RumbleClip::Create(
        Haptics::RumbleClipData{.Duration = 1.0f, .Loop = true, .LowFrequency = flat}));

    Haptics::HapticsEngine engine;
    engine.Play(Haptics::RumbleTarget::ForGamepad(static_cast<GamepadId>(2)), clip);
    engine.Play(Haptics::RumbleTarget::ForSeat(
                    SeatRef{.World = WorldInstanceId{.Value = 3}, .Viewer = Entity{.Index = 1}}),
                clip, Haptics::RumbleParams{.Intensity = 0.5f});
    engine.Update({.Delta = 0.0f});

    {
        const Mcp::McpHost host{
            .Types = registry, .Assets = *assets, .Haptics = [&engine] { return &engine; }};
        Mcp::McpServerInfo info;
        info.Port = 0;
        const Unique<Mcp::McpServer> server = Mcp::McpServer::Create(info, host);
        Serve(*server,
              [](httplib::Client& client)
              {
                  const Json result = CallTool(client, "haptics.state");
                  Check(result.is_object() && !result.value("isError", false),
                        "haptics.state answers on a read-only server");
                  const Json state = Json::parse(result["content"][0].value("text", std::string{}),
                                                 nullptr, false);

                  Check(state.value("master_intensity", 0.0) == 1.0, "master intensity is 1");
                  Check(!state.value("suspended", true), "output is not suspended");

                  const Json& pads = state["pads"];
                  Check(pads.is_array() && pads.size() == 1, "only the playing pad is reported");
                  if (pads.size() == 1)
                  {
                      Check(pads[0].value("pad", -1) == 2, "the playing pad is slot 2");
                      Check(pads[0]["output"].value("low_frequency", 0.0) == 0.5,
                            "slot 2 mixes the clip's level");
                  }

                  const Json& instances = state["instances"];
                  Check(instances.is_array() && instances.size() == 2, "both instances reported");
                  if (instances.size() == 2)
                  {
                      Check(instances[0]["target"].value("kind", "") == "gamepad",
                            "the first instance targets a pad");
                      Check(instances[0].value("pad", -1) == 2, "and resolved to slot 2");
                      Check(instances[1]["target"].value("kind", "") == "seat",
                            "the second instance targets a seat");
                      Check(instances[1]["pad"].is_null(), "a padless seat resolves to no pad");
                      Check(instances[1].value("intensity", 0.0) == 0.5,
                            "the seat instance carries its intensity");
                      Check(instances[1]["world"].is_null(), "an application-owned instance");
                  }
              });
    }

    // A host with no Haptics reports the engine unavailable rather than dereferencing it.
    {
        const Mcp::McpHost host{.Types = registry, .Assets = *assets};
        Mcp::McpServerInfo info;
        info.Port = 0;
        const Unique<Mcp::McpServer> server = Mcp::McpServer::Create(info, host);
        Serve(*server,
              [](httplib::Client& client)
              {
                  const Json result = CallTool(client, "haptics.state");
                  Check(result.is_object() && result.value("isError", false),
                        "haptics.state with no Haptics host is a tool error");
              });
    }

    if (g_Failures == 0)
    {
        std::printf("mcp_haptics: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "mcp_haptics: %d check(s) failed\n", g_Failures);
    return 1;
}
