// Device-free proof for the editor reflection seam + the generic editor MCP tools.
//
// A test-only AssetEditorPanel exposes a LevelRenderSettings inspectable and carries a real
// CommandStack over a small Scene, standing in for a level editor without a live device. Wired
// through an EditorMcpHost + the world mutation tools' ApplyMutation routing, driven over
// loopback, it exercises: editor.list_panels (the panel + its inspectable names), editor.inspect
// (round-trips a render-settings value), editor.set_field (marks the document dirty and the write
// takes), and entity.add_component through the routed command path with editor.undo removing it
// (command-stack routing works). Pure logic + loopback, no GPU — the default band, following the
// tests/editor/ device-free precedent.

#include "EditorMcp.h"

#include "AssetEditorPanel.h"
#include "CommandStack.h"
#include "panels/PrefabEditContext.h"

#include <VengEditor/EditorPanel.h>

#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpServerInfo.h>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#define CPPHTTPLIB_IMPLEMENTATION
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using Json = nlohmann::json;
using namespace Veng;

namespace
{
    // A minimal AssetEditorPanel standing in for the level editor: it owns a Scene + a
    // PrefabEditContext + a CommandStack (the routing target) and exposes the render settings as an
    // inspectable, tracking whether OnInspectableChanged fired (the panel's "mark dirty" reaction).
    class TestDocumentPanel final : public VengEditor::AssetEditorPanel
    {
    public:
        explicit TestDocumentPanel(Scene& scene)
        {
            m_Ctx.Scene = &scene;
            m_Ctx.Assets = nullptr;
            m_Commands = CreateUnique<VengEditor::CommandStack>(m_Ctx);
        }

        [[nodiscard]] string_view GetTitle() const override { return "Level"; }

        [[nodiscard]] vector<VengEditor::Inspectable> GetInspectables() override
        {
            return {VengEditor::Inspectable{.Name = "renderSettings",
                                            .Type = TypeIdOf<LevelRenderSettings>(),
                                            .Data = &m_Render}};
        }

        void OnInspectableChanged(string_view name) override
        {
            if (name == "renderSettings")
            {
                m_Dirty = true;
            }
        }

        [[nodiscard]] VengEditor::CommandStack* GetCommandStack() override
        {
            return m_Commands.get();
        }

        [[nodiscard]] bool HasUnsavedChanges() const override
        {
            return m_Dirty || m_Commands->IsDirty();
        }

    protected:
        void BuildDefaultLayout(u32) override {}

    private:
        VengEditor::PrefabEditContext m_Ctx;
        Unique<VengEditor::CommandStack> m_Commands;
        LevelRenderSettings m_Render;
        bool m_Dirty = false;
    };

    int g_Id = 100;

    Json Post(httplib::Client& client, const Json& message)
    {
        const httplib::Result res = client.Post("/", message.dump(), "application/json");
        if (!res)
        {
            return Json{{"error", "no response"}};
        }
        return Json::parse(res->body, nullptr, false);
    }

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
}

TEST_CASE("editor MCP reflection seam + command routing")
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);

    Unique<Scene> scene = Scene::Create(registry);
    const Entity entity = scene->CreateEntity();

    TestDocumentPanel panel(*scene);

    // The editor provider seam: one open panel, itself the focused document, over the doc scene.
    const VengEditor::EditorMcpHost editorHost{
        .Types = registry,
        .Panels = [&] { return vector<VengEditor::EditorPanel*>{&panel}; },
        .FocusedDocument = [&] { return static_cast<VengEditor::AssetEditorPanel*>(&panel); },
        .DocumentScene = [&] { return scene.get(); }};

    // The world tools reach the same document scene; ApplyMutation routes a mutation onto the
    // focused document's command stack (undoable) exactly as an editor host wires it.
    AssetManager* assets = nullptr;
    const Mcp::McpHost host{.Types = registry,
                            .Assets = *assets,
                            .CurrentWorld = [&] { return scene.get(); },
                            .ApplyMutation = [&](const Mcp::McpMutation& mutation)
                            { return VengEditor::ApplyEditorMutation(editorHost, mutation); }};

    Mcp::McpServerInfo info;
    info.Port = 0;
    info.AllowMutations = true;

    Unique<Mcp::McpServer> server = Mcp::McpServer::Create(info, host);
    VengEditor::RegisterEditorReflectionTools(*server, editorHost);
    const u16 port = server->GetPort();
    REQUIRE(port != 0);

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

        // editor.list_panels reports the panel, its kind, focus, and its inspectable names.
        const Json panels = Payload(CallTool(client, "editor.list_panels", Json::object()));
        REQUIRE(panels.is_object());
        REQUIRE(panels["panels"].size() == 1);
        const Json& listed = panels["panels"][0];
        CHECK(listed.value("title", std::string{}) == "Level");
        CHECK(listed.value("kind", std::string{}) == "assetEditor");
        CHECK(listed.value("focused", false) == true);
        REQUIRE(listed["inspectables"].size() == 1);
        CHECK(listed["inspectables"][0].get<std::string>() == "renderSettings");

        // editor.inspect round-trips the render settings' default Exposure.
        const Json inspected = Payload(CallTool(
            client, "editor.inspect", Json{{"panel", "Level"}, {"inspectable", "renderSettings"}}));
        REQUIRE(inspected.is_object());
        CHECK(inspected.value("name", std::string{}) == "renderSettings");
        CHECK(std::abs(inspected["fields"]["Exposure"].get<f32>() - 1.0f) < 1e-5f);
        CHECK(inspected["fields"]["Bloom"].get<bool>() == true);

        // editor.set_field writes a field and fires the panel's reaction (marks it dirty).
        const Json set =
            Payload(CallTool(client, "editor.set_field",
                             Json{{"panel", "Level"},
                                  {"inspectable", "renderSettings"},
                                  {"values", {{"Exposure", 2.5f}, {"Bloom", false}}}}));
        REQUIRE(set.is_object());
        CHECK(set.value("inspectable", std::string{}) == "renderSettings");

        // The write took, read back through editor.inspect.
        const Json afterSet = Payload(CallTool(
            client, "editor.inspect", Json{{"panel", "Level"}, {"inspectable", "renderSettings"}}));
        CHECK(std::abs(afterSet["fields"]["Exposure"].get<f32>() - 2.5f) < 1e-5f);
        CHECK(afterSet["fields"]["Bloom"].get<bool>() == false);
        CHECK(panel.HasUnsavedChanges() == true);

        // An unknown panel / inspectable is an isError, never a fatal path.
        const Json badPanel = CallTool(client, "editor.inspect", Json{{"panel", "Nope"}});
        CHECK(badPanel.value("isError", false) == true);
        const Json badField =
            CallTool(client, "editor.inspect", Json{{"panel", "Level"}, {"inspectable", "nope"}});
        CHECK(badField.value("isError", false) == true);

        // entity.add_component routes through ApplyEditorMutation onto the command stack: the Light
        // is added and the document is dirty, and editor.undo removes it (command-stack routing).
        const Json spawnId = Json{{"index", entity.Index}, {"generation", entity.Generation}};
        const Json added = Payload(CallTool(client, "entity.add_component",
                                            Json{{"id", spawnId},
                                                 {"component", "Veng::Light"},
                                                 {"values", {{"Intensity", 3.0f}}}}));
        CHECK(added.value("added", std::string{}) == "Veng::Light");
        CHECK(scene->Has<Light>(entity));
        CHECK(std::abs(scene->Get<Light>(entity).Intensity - 3.0f) < 1e-5f);
        CHECK(panel.GetCommandStack()->CanUndo());

        // editor.undo reverts the routed add — the Light is gone.
        const Json undone = Payload(CallTool(client, "editor.undo", Json::object()));
        REQUIRE(undone.is_object());
        CHECK(undone.value("undone", false) == true);
        CHECK(!scene->Has<Light>(entity));

        // editor.redo re-applies it.
        const Json redone = Payload(CallTool(client, "editor.redo", Json::object()));
        CHECK(redone.value("redone", false) == true);
        CHECK(scene->Has<Light>(entity));

        // editor.save routes to the focused document's Save; the base AssetEditorPanel::Save reports
        // no save action, surfaced as an isError.
        const Json saved = CallTool(client, "editor.save", Json::object());
        CHECK(saved.value("isError", false) == true);
    }

    done.store(true);
    pump.join();
}
