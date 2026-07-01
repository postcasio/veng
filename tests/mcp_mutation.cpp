// Headless proof for the veng::mcp mutation tools + the JsonToFields write-side walk.
//
// Constructs an McpServer with AllowMutations = true and a null ApplyMutation (the raw-scene
// path), wires an McpHost over a small device-free Scene, drives Pump() on this (render) thread,
// and over loopback exercises entity.spawn / entity.add_component / entity.get (round-trips
// JsonToFields → FieldsToJson) / entity.set_field / entity.remove_component / entity.destroy —
// asserting each edit took, that a stale handle errors, and that the spatial version moved. A
// second server with AllowMutations = false asserts the mutation tools are absent from
// tools/list. Pure logic + loopback, no GPU (the scene-only ops touch no Assets), so it runs in
// the default band.

#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpServerInfo.h>
#include <Veng/Mcp/McpTool.h>

#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <nlohmann/json.hpp>

#define CPPHTTPLIB_IMPLEMENTATION
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cmath>
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

    Json CallTool(httplib::Client& client, const std::string& name, const Json& args)
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
}

int main()
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);

    Unique<Scene> scene = Scene::Create(registry);

    // The mutation tools never touch Assets on the scene-only paths this test exercises; bind a
    // never-dereferenced pointer (an AssetManager needs a render Context this device-free test
    // has none of), exactly as mcp_world does.
    AssetManager* assets = nullptr;
    const Mcp::McpHost host{
        .Types = registry, .Assets = *assets, .CurrentWorld = [&] { return scene.get(); }};

    Mcp::McpServerInfo info;
    info.Port = 0;
    info.AllowMutations = true;

    Unique<Mcp::McpServer> server = Mcp::McpServer::Create(info, host);
    const u16 port = server->GetPort();
    Check(port != 0, "GetPort resolved an ephemeral port");

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

        // tools/list surfaces the six mutation tools (AllowMutations is on).
        const std::set<std::string> names = ToolNames(client);
        Check(names.count("entity.add_component") == 1, "entity.add_component registered");
        Check(names.count("entity.remove_component") == 1, "entity.remove_component registered");
        Check(names.count("entity.set_field") == 1, "entity.set_field registered");
        Check(names.count("entity.spawn") == 1, "entity.spawn registered");
        Check(names.count("entity.destroy") == 1, "entity.destroy registered");
        Check(names.count("world.load_prefab") == 1, "world.load_prefab registered");

        const u64 versionBefore = scene->GetSpatialVersion();

        // entity.spawn creates a named entity carrying a Transform seeded from partial values.
        const Json spawned = Payload(CallTool(
            client, "entity.spawn",
            Json{{"name", "Agent"},
                 {"components",
                  {{"Veng::Transform", {{"Position", Json::array({1.0f, 2.0f, 3.0f})}}}}}}));
        Check(spawned.contains("id"), "entity.spawn returns a new id");
        const u32 spawnIndex = spawned["id"].value("index", 0u);
        const u32 spawnGen = spawned["id"].value("generation", 0u);
        const Json spawnId = Json{{"index", spawnIndex}, {"generation", spawnGen}};

        Check(scene->EntityCount() == 1, "spawn created exactly one entity");
        Check(scene->GetSpatialVersion() > versionBefore,
              "a spawn with a Transform moved the spatial version");

        // entity.get round-trips the spawned Name + Transform through the reflection bridge.
        const Json got = Payload(CallTool(client, "entity.get", Json{{"id", spawnId}}));
        Check(got.value("name", std::string{}) == "Agent", "spawned entity carries the Name");
        const Json& transform = got["components"]["Veng::Transform"];
        Check(std::abs(transform["Position"][0].get<f32>() - 1.0f) < 1e-5f,
              "spawned Transform.Position.x round-trips (JsonToFields → FieldsToJson)");
        Check(std::abs(transform["Position"][2].get<f32>() - 3.0f) < 1e-5f,
              "spawned Transform.Position.z round-trips");

        // entity.add_component adds a Light and seeds its enum + scalar fields.
        const Json added =
            Payload(CallTool(client, "entity.add_component",
                             Json{{"id", spawnId},
                                  {"component", "Veng::Light"},
                                  {"values", {{"Type", "Point"}, {"Intensity", 2.5f}}}}));
        Check(added.value("added", std::string{}) == "Veng::Light",
              "entity.add_component reports the added Light");

        const Json afterAdd = Payload(CallTool(client, "entity.get", Json{{"id", spawnId}}));
        const Json& light = afterAdd["components"]["Veng::Light"];
        Check(light["Type"].value("name", std::string{}) == "Point",
              "add_component seeded the Light enum by name");
        Check(std::abs(light["Intensity"].get<f32>() - 2.5f) < 1e-5f,
              "add_component seeded the Light scalar");

        // Adding the same component again errors (already present) — the process stays alive.
        const Json dupAdd = CallTool(client, "entity.add_component",
                                     Json{{"id", spawnId}, {"component", "Veng::Light"}});
        Check(dupAdd.value("isError", false) == true, "adding a duplicate component is an isError");

        // An unregistered component name is an isError, never a fatal assert.
        const Json badAdd = CallTool(client, "entity.add_component",
                                     Json{{"id", spawnId}, {"component", "No::Such::Type"}});
        Check(badAdd.value("isError", false) == true, "an unregistered component is an isError");

        // entity.set_field applies a partial update; the omitted enum keeps its value.
        CallTool(
            client, "entity.set_field",
            Json{{"id", spawnId}, {"component", "Veng::Light"}, {"values", {{"Intensity", 9.0f}}}});
        const Json afterSet = Payload(CallTool(client, "entity.get", Json{{"id", spawnId}}));
        const Json& light2 = afterSet["components"]["Veng::Light"];
        Check(std::abs(light2["Intensity"].get<f32>() - 9.0f) < 1e-5f,
              "set_field updated the Light intensity");
        Check(light2["Type"].value("name", std::string{}) == "Point",
              "set_field left the omitted Light enum unchanged (partial update)");

        // A set_field with a type-mismatched value is a located error, not a silent skip.
        const Json badSet = CallTool(client, "entity.set_field",
                                     Json{{"id", spawnId},
                                          {"component", "Veng::Light"},
                                          {"values", {{"Intensity", "not-a-number"}}}});
        Check(badSet.value("isError", false) == true, "a type-mismatched value is an isError");

        // entity.remove_component removes the Light; the Hierarchy component is not removable.
        const Json removed = Payload(CallTool(client, "entity.remove_component",
                                              Json{{"id", spawnId}, {"component", "Veng::Light"}}));
        Check(removed.value("removed", std::string{}) == "Veng::Light",
              "entity.remove_component reports the removed Light");
        const Json afterRemove = Payload(CallTool(client, "entity.get", Json{{"id", spawnId}}));
        Check(!afterRemove["components"].contains("Veng::Light"),
              "the Light is gone after remove_component");

        // A spawned child parented under the agent, then a destroy of the parent takes both.
        const Json child =
            Payload(CallTool(client, "entity.spawn", Json{{"name", "Child"}, {"parent", spawnId}}));
        Check(child.contains("id"), "the child spawned under a parent");
        Check(scene->EntityCount() == 2, "two entities live before the recursive destroy");

        const Json destroyed = Payload(CallTool(client, "entity.destroy", Json{{"id", spawnId}}));
        Check(destroyed.value("destroyed", 0) == 2, "entity.destroy reports the subtree count");
        Check(scene->EntityCount() == 0, "the parent and child are both destroyed");

        // The now-stale parent handle is an isError, never a silent stale-handle access.
        const Json stale = CallTool(client, "entity.get", Json{{"id", spawnId}});
        Check(stale.value("isError", false) == true, "the destroyed handle now errors");
    }

    done.store(true);
    pump.join();

    // A second server with AllowMutations off exposes none of the mutation tools.
    {
        Mcp::McpServerInfo readOnly;
        readOnly.Port = 0;
        readOnly.AllowMutations = false;

        Unique<Mcp::McpServer> roServer = Mcp::McpServer::Create(readOnly, host);
        const u16 roPort = roServer->GetPort();

        std::atomic<bool> roDone{false};
        std::thread roPump(
            [&]
            {
                while (!roDone.load())
                {
                    roServer->Pump();
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                roServer->Pump();
            });

        {
            httplib::Client client("127.0.0.1", roPort);
            client.set_connection_timeout(5, 0);
            client.set_read_timeout(10, 0);
            const std::set<std::string> names = ToolNames(client);
            Check(names.count("entity.add_component") == 0,
                  "a read-only server omits entity.add_component");
            Check(names.count("entity.set_field") == 0,
                  "a read-only server omits entity.set_field");
            Check(names.count("entity.destroy") == 0, "a read-only server omits entity.destroy");
            Check(names.count("world.load_prefab") == 0,
                  "a read-only server omits world.load_prefab");
            // The read-only inspection tools stay present.
            Check(names.count("entity.get") == 1, "a read-only server keeps entity.get");
        }

        roDone.store(true);
        roPump.join();
    }

    if (g_Failures == 0)
    {
        std::printf("mcp_mutation: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "mcp_mutation: %d check(s) failed\n", g_Failures);
    return 1;
}
