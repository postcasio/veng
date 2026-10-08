// Headless proof for the read-only haptics tool (haptics.state).
//
// Stands an McpServer up (read-only, the default) over an McpHost whose Haptics closure resolves a
// HapticsEngine with no device, plays one one-shot on pad 2 in the always-Live application scope and
// one on pad 1 in a scope left unrenewed (Held), submits a layer on pad 2, mixes one frame, and reads
// haptics.state over loopback: the playing pad is reported with its mixed level and its counts, each
// one-shot and the layer carry their scope and its state, and idle slots are left out. A second
// server whose host leaves Haptics null reports the tool unavailable. The engine is mutated only
// before the pump starts, so the pump thread only reads it. Pure logic + loopback, no GPU, so it
// runs in the default band.

#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpServerInfo.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Haptics/Haptics.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/PresentationScope.h>

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

    PresentationScopes scopes;
    const Unique<PresentationScope> held = scopes.Open();
    Haptics::HapticsEngine engine(scopes);
    engine.PlayOneShot(scopes.GetApplicationScope(), static_cast<GamepadId>(2), clip);
    engine.PlayOneShot(held->GetId(), static_cast<GamepadId>(1), clip, 0.5f);
    engine.SubmitLayer(scopes.GetApplicationScope(), static_cast<GamepadId>(2),
                       Haptics::RumbleChannels{.HighFrequency = 0.25f});
    scopes.Resolve();
    engine.Update({.Delta = 0.0f});

    {
        const Mcp::McpHost host{
            .Types = registry, .Assets = *assets, .Haptics = [&engine] { return &engine; }};
        Mcp::McpServerInfo info;
        info.Port = 0;
        const Unique<Mcp::McpServer> server = Mcp::McpServer::Create(info, host);
        Serve(
            *server,
            [](httplib::Client& client)
            {
                const Json result = CallTool(client, "haptics.state");
                Check(result.is_object() && !result.value("isError", false),
                      "haptics.state answers on a read-only server");
                const Json state =
                    Json::parse(result["content"][0].value("text", std::string{}), nullptr, false);

                Check(state.value("master_intensity", 0.0) == 1.0, "master intensity is 1");
                Check(!state.value("suspended", true), "output is not suspended");

                const Json& pads = state["pads"];
                Check(pads.is_array() && pads.size() == 2, "only the two pads in use are reported");
                if (pads.size() == 2)
                {
                    Check(pads[0].value("pad", -1) == 1 && pads[1].value("pad", -1) == 2,
                          "the pads in use are slots 1 and 2");
                    Check(pads[0]["output"].value("low_frequency", 1.0) == 0.0,
                          "slot 1's one-shot is held, so it mixes nothing");
                    Check(pads[1]["output"].value("low_frequency", 0.0) == 0.5,
                          "slot 2 mixes the live one-shot's level");
                    Check(pads[1]["output"].value("high_frequency", 0.0) == 0.25,
                          "slot 2 mixes the layer's level");
                    Check(pads[1].value("one_shot_count", 0) == 1 &&
                              pads[1].value("layer_count", 0) == 1,
                          "slot 2 counts one one-shot and one layer");
                }

                const Json& shots = state["one_shots"];
                Check(shots.is_array() && shots.size() == 2, "both one-shots reported");
                if (shots.size() == 2)
                {
                    Check(shots[0].value("state", "") == "live" && shots[0].value("pad", -1) == 2,
                          "the application scope's one-shot is live on slot 2");
                    Check(shots[1].value("state", "") == "held" && shots[1].value("pad", -1) == 1,
                          "the unrenewed scope's one-shot is held on slot 1");
                    Check(shots[1].value("intensity", 0.0) == 0.5,
                          "the held one-shot carries its intensity");
                    Check(shots[0].value("scope", 0ULL) != shots[1].value("scope", 0ULL),
                          "each one-shot names its own scope");
                }

                const Json& layers = state["layers"];
                Check(layers.is_array() && layers.size() == 1, "the mixed layer is reported");
                if (layers.size() == 1)
                {
                    Check(layers[0].value("state", "") == "live" && layers[0].value("pad", -1) == 2,
                          "the layer is live on slot 2");
                    Check(layers[0]["channels"].value("high_frequency", 0.0) == 0.25,
                          "the layer carries its channels");
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
