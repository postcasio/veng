// Headless proof for the veng::mcp read-only world tools + the ReflectToJson walk.
//
// Builds a TypeRegistry with the engine builtins plus a test component spanning
// Scalar/Vector/Enum/AssetHandle/Struct, spawns a small Scene, wires an McpHost returning
// it, drives Pump() on this (render) thread, and over loopback exercises
// world.list_entities / entity.get / world.query / scene.stats — asserting entity counts,
// that a known component's field values round-trip through FieldsToJson, the component
// filter, an unregistered-type isError (process stays alive), and pagination walking the
// full entity set exactly once. Pure logic + loopback, no GPU, so it runs in the default band.

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
#include <cstring>
#include <set>
#include <string>
#include <thread>

using Json = nlohmann::json;
using namespace Veng;

namespace Test
{
    /// @brief A nested struct field to exercise ReflectToJson's Struct recursion.
    struct Stats
    {
        f32 Health = 0.0f;
        u32 Level = 0;
    };

    /// @brief A test component spanning Scalar / Vector / Enum / AssetHandle / Struct.
    struct Widget
    {
        f32 Speed = 0.0f;
        vec3 Tint{0.0f};
        LightType Kind = LightType::Directional;
        AssetHandle<Mesh> Model;
        Stats Stat;
    };
}

VE_REFLECT(::Test::Stats, 0x1111111100000001ULL)
VE_FIELD(Health)
VE_FIELD(Level)
VE_REFLECT_END();

VE_REFLECT(::Test::Widget, 0x1111111100000002ULL)
VE_FIELD(Speed)
VE_FIELD(Tint)
VE_FIELD(Kind)
VE_FIELD(Model)
VE_FIELD(Stat)
VE_REFLECT_END();

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

    // Calls a tool over loopback and returns the parsed tool-result envelope.
    Json CallTool(httplib::Client& client, const std::string& name, const Json& args)
    {
        const Json response = Post(client, Json{{"jsonrpc", "2.0"},
                                                {"id", g_Id++},
                                                {"method", "tools/call"},
                                                {"params", {{"name", name}, {"arguments", args}}}});
        return response.contains("result") ? response["result"] : Json(nullptr);
    }

    // Parses a successful tool result's text payload back into JSON.
    Json Payload(const Json& result)
    {
        if (!result.is_object() || !result.contains("content"))
        {
            return Json(nullptr);
        }
        return Json::parse(result["content"][0].value("text", std::string{}), nullptr, false);
    }
}

int main()
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    registry.Register<Test::Widget>();

    Unique<Scene> scene = Scene::Create(registry);

    // A named entity carrying the test Widget with known field values, to round-trip
    // ReflectToJson across Scalar/Vector/Enum/AssetHandle/Struct.
    const Entity hero = scene->CreateEntity();
    scene->Add<Name>(hero, Name{.Value = "Hero"});
    Test::Widget widget;
    widget.Speed = 2.5f;
    widget.Tint = vec3(0.1f, 0.2f, 0.3f);
    widget.Kind = LightType::Spot;
    // The AssetHandle's leading AssetId sits at offset 0 (pinned by AssetHandleLayoutGuard);
    // its value constructor is private, so seed the id the way the reflection serializer writes it
    // — through a byte pointer, since AssetHandle<T> is not trivially copyable as a whole.
    const u64 modelId = 0x1234ABCDULL;
    std::memcpy(static_cast<void*>(&widget.Model), &modelId, sizeof(modelId));
    widget.Stat = Test::Stats{.Health = 42.0f, .Level = 7};
    scene->Add<Test::Widget>(hero, widget);

    // A second entity holding only a Light — the world.query / filter target.
    const Entity lamp = scene->CreateEntity();
    scene->Add<Name>(lamp, Name{.Value = "Lamp"});
    scene->Add<Light>(lamp, Light{.Type = LightType::Point});

    // A third, bare entity (no Name) to prove the listing tolerates a missing Name.
    const Entity bare = scene->CreateEntity();

    Mcp::McpServerInfo info;
    info.Port = 0;

    // The world tools in this plan never touch Assets; bind it through a never-dereferenced
    // pointer (an AssetManager needs a render Context this device-free test has none of).
    AssetManager* assets = nullptr;
    const Mcp::McpHost host{.Types = registry,
                            .Assets = *assets,
                            .CurrentWorld = [&] { return scene.get(); },
                            .Viewport = {}};

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

        // tools/list surfaces the four auto-registered world tools.
        const Json list = Post(client, Json{{"jsonrpc", "2.0"},
                                            {"id", 1},
                                            {"method", "tools/list"},
                                            {"params", Json::object()}});
        std::set<std::string> names;
        for (const Json& tool : list["result"]["tools"])
        {
            names.insert(tool.value("name", std::string{}));
        }
        Check(names.count("world.list_entities") == 1, "world.list_entities registered");
        Check(names.count("entity.get") == 1, "entity.get registered");
        Check(names.count("world.query") == 1, "world.query registered");
        Check(names.count("scene.stats") == 1, "scene.stats registered");

        // scene.stats reports the three live entities.
        const Json stats = Payload(CallTool(client, "scene.stats", Json::object()));
        Check(stats.value("entity_count", 0) == 3, "scene.stats counts three entities");
        Check(stats.contains("spatial_version"), "scene.stats reports a spatial version");

        // world.list_entities lists all three, with the hero's component types.
        const Json all = Payload(CallTool(client, "world.list_entities", Json::object()));
        Check(all["entities"].size() == 3, "world.list_entities lists all three entities");
        bool sawHeroComponents = false;
        for (const Json& entity : all["entities"])
        {
            if (entity.value("name", std::string{}) == "Hero")
            {
                for (const Json& comp : entity["components"])
                {
                    if (comp.get<std::string>() == "Test::Widget")
                    {
                        sawHeroComponents = true;
                    }
                }
            }
        }
        Check(sawHeroComponents, "hero lists its Test::Widget component");

        // The component filter narrows to the Light-bearing entity.
        const Json filtered =
            Payload(CallTool(client, "world.list_entities", Json{{"component", "Veng::Light"}}));
        Check(filtered["entities"].size() == 1, "filter narrows to the one Light entity");
        Check(filtered["entities"][0].value("name", std::string{}) == "Lamp",
              "filter selects the Lamp");

        // world.query is the same set, ids + names only.
        const Json queried =
            Payload(CallTool(client, "world.query", Json{{"component", "Veng::Light"}}));
        Check(queried["entities"].size() == 1, "world.query returns the Light entity");

        // entity.get dumps the hero's components; ReflectToJson round-trips the Widget.
        const Json got = Payload(
            CallTool(client, "entity.get",
                     Json{{"id", {{"index", hero.Index}, {"generation", hero.Generation}}}}));
        Check(got.value("name", std::string{}) == "Hero", "entity.get returns the hero name");
        const Json& components = got["components"];
        Check(components.contains("Test::Widget"), "entity.get dumps the Widget");
        const Json& w = components["Test::Widget"];
        Check(std::abs(w.value("Speed", 0.0f) - 2.5f) < 1e-5f, "Widget Speed round-trips (Scalar)");
        Check(w["Tint"].is_array() && w["Tint"].size() == 3, "Widget Tint is a 3-array (Vector)");
        Check(std::abs(w["Tint"][1].get<f32>() - 0.2f) < 1e-5f, "Widget Tint.y round-trips");
        Check(w["Kind"].value("name", std::string{}) == "Spot", "Widget Kind names its enumerator");
        Check(w["Kind"].value("value", -1) == static_cast<int>(LightType::Spot),
              "Widget Kind carries the raw enum value");
        Check(w["Model"].get<std::string>() == std::to_string(0x1234ABCDULL),
              "Widget Model is the decimal AssetId (AssetHandle)");
        Check(w["Stat"].is_object() && w["Stat"].value("Level", 0) == 7,
              "Widget Stat recurses into the nested Struct");

        // An unregistered component name is an isError result — the process stays alive
        // (guards the fatal-assert path on an unknown agent-supplied type).
        const Json bad = CallTool(client, "world.query", Json{{"component", "No::Such::Type"}});
        Check(bad.value("isError", false) == true, "unregistered type is an isError result");

        const Json badList =
            CallTool(client, "world.list_entities", Json{{"component", "No::Such::Type"}});
        Check(badList.value("isError", false) == true,
              "unregistered filter is an isError on list too");

        // A stale entity handle is an isError, never a silent stale-handle access.
        const Json stale = CallTool(client, "entity.get",
                                    Json{{"id", {{"index", hero.Index}, {"generation", 999}}}});
        Check(stale.value("isError", false) == true, "a stale entity handle is an isError");

        // Pagination: a limit of 1 over the 3-entity scene returns a nextCursor, and paging
        // through walks the full set exactly once with no duplicates or gaps.
        std::set<int> seen;
        Json args = Json{{"limit", 1}};
        int pages = 0;
        for (;;)
        {
            const Json page = Payload(CallTool(client, "world.list_entities", args));
            for (const Json& entity : page["entities"])
            {
                const int index = entity["id"].value("index", -1);
                Check(seen.insert(index).second, "paginated entity is not seen twice");
            }
            ++pages;
            if (!page.contains("nextCursor"))
            {
                break;
            }
            args = Json{{"limit", 1}, {"cursor", page["nextCursor"]}};
            Check(pages < 10, "pagination terminates");
            if (pages >= 10)
            {
                break;
            }
        }
        Check(seen.size() == 3, "pagination walked all three entities");
        Check(seen.count(static_cast<int>(bare.Index)) == 1, "pagination reached the bare entity");
    }

    done.store(true);
    pump.join();

    if (g_Failures == 0)
    {
        std::printf("mcp_world: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "mcp_world: %d check(s) failed\n", g_Failures);
    return 1;
}
