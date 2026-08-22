// Headless proof for the veng::mcp gui inspection tools.
//
// Builds a Document by hand — a header row above a scrolling list of rows, each row a Button
// holding a Text — injects it into a GuiSurface on a scene entity, solves it against a device-free
// text measurer, and over loopback exercises gui.list_documents / gui.inspect. What it asserts is
// the property the tools exist for: a reported rect is the element's *solved* box, so a child's
// box lies inside its parent's and a row that follows a taller sibling is reported lower than one
// that does not. Pure logic + loopback, no GPU, so it runs in the default band.

#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpServerInfo.h>

#include <Veng/Gui/Document.h>
#include <Veng/Gui/Element.h>
#include <Veng/Gui/Surface.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <nlohmann/json.hpp>

#define CPPHTTPLIB_IMPLEMENTATION
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <set>
#include <string>
#include <thread>

using Json = nlohmann::json;
using namespace Veng;

namespace
{
    int g_Failures = 0;

    void Check(const bool condition, const char* const what)
    {
        if (condition)
        {
            std::printf("ok: %s\n", what);
            return;
        }
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_Failures;
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

    /// @brief Finds the first node in a reported tree carrying an id.
    const Json* FindNode(const Json& node, const std::string& id)
    {
        if (node.value("id", std::string{}) == id)
        {
            return &node;
        }
        if (!node.contains("children"))
        {
            return nullptr;
        }
        for (const Json& child : node["children"])
        {
            if (const Json* const found = FindNode(child, id); found != nullptr)
            {
                return found;
            }
        }
        return nullptr;
    }

    /// @brief A document shaped like a list with a heading over its first row.
    ///
    /// The heading is what makes the tree worth inspecting: it occupies layout height, so the row
    /// beneath it must solve lower than a row with no heading above it, and every row's label must
    /// sit inside the row that owns it. Those are the two claims a caller reaches for these tools
    /// to settle.
    Unique<Gui::Document> BuildListDocument()
    {
        auto document = CreateUnique<Gui::Document>();
        // Device-free measurement: a run is its character count at a fixed advance, one line tall.
        // The solver needs an intrinsic size for a Text leaf and a test has no font.
        document->SetTextMeasurer(
            [](const string_view text, const Gui::Style& style, optional<f32>) -> vec2
            {
                return vec2{static_cast<f32>(text.size()) * 8.0f,
                            style.TextSize > 0.0f ? style.TextSize : 16.0f};
            });

        Gui::Element& root = document->Root();
        Gui::Style column;
        column.Direction = Gui::FlexDirection::Column;
        document->SetStyle(root, column);

        for (u32 row = 0; row < 3; ++row)
        {
            Gui::Element& item = document->Add(root, Gui::ElementKind::Panel);
            document->SetStyle(item, column);
            item.Id = "item-" + std::to_string(row);

            // Only the first row is headed, which is the asymmetry the rects have to show.
            if (row == 0)
            {
                Gui::Element& caption = document->Add(item, Gui::ElementKind::Text);
                caption.Id = "caption";
                document->SetText(caption, "METALS");
            }

            Gui::Element& button = document->Add(item, Gui::ElementKind::Button);
            button.Id = "row-" + std::to_string(row);
            Gui::Style rowStyle;
            rowStyle.Direction = Gui::FlexDirection::Row;
            rowStyle.Height = Gui::Length::Points(24.0f);
            document->SetStyle(button, rowStyle);

            Gui::Element& label = document->Add(button, Gui::ElementKind::Text);
            label.Id = "label-" + std::to_string(row);
            document->SetText(label, "IRON ORE");
        }

        document->Solve(vec2{400.0f, 300.0f});
        return document;
    }
}

int main()
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);

    Unique<Scene> scene = Scene::Create(registry);

    const Entity panel = scene->CreateEntity();
    scene->Add<Name>(panel, Name{.Value = "Console"});
    GuiSurface surface;
    surface.SetDocument(BuildListDocument());
    scene->Add<GuiSurface>(panel, std::move(surface));

    // A second entity with no document at all, so resolving by entity has something to refuse.
    const Entity bare = scene->CreateEntity();
    scene->Add<Name>(bare, Name{.Value = "Bare"});

    Mcp::McpServerInfo info;
    info.Port = 0;

    // The gui tools never touch Assets; bind it through a never-dereferenced pointer, as the
    // world-tools proof does (an AssetManager needs a render Context this test has none of).
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

        const Json list = Post(client, Json{{"jsonrpc", "2.0"},
                                            {"id", 1},
                                            {"method", "tools/list"},
                                            {"params", Json::object()}});
        std::set<std::string> names;
        for (const Json& tool : list["result"]["tools"])
        {
            names.insert(tool.value("name", std::string{}));
        }
        Check(names.count("gui.list_documents") == 1, "gui.list_documents registered");
        Check(names.count("gui.inspect") == 1, "gui.inspect registered");

        // The listing names the one document the scene presents, and the entity that owns it.
        const Json documents = Payload(CallTool(client, "gui.list_documents", Json::object()));
        Check(documents["documents"].size() == 1, "one document is presented");
        const Json& entry = documents["documents"][0];
        Check(entry.value("kind", std::string{}) == "surface", "it is reported as a surface");
        Check(entry.value("name", std::string{}) == "Console", "the owning entity is named");
        Check(entry["canvas"].value("w", 0.0) > 0.0, "the canvas carries a solved width");

        // The tree, deep enough to reach the labels inside the rows.
        const Json tree = Payload(CallTool(client, "gui.inspect", Json{{"depth", 8}}));
        const Json& root = tree["root"];
        Check(tree.value("elements", 0) > 0, "the walk reported an element count");

        const Json* const headedRow = FindNode(root, "row-0");
        const Json* const plainRow = FindNode(root, "row-1");
        const Json* const headedLabel = FindNode(root, "label-0");
        const Json* const caption = FindNode(root, "caption");
        Check(headedRow != nullptr && plainRow != nullptr && headedLabel != nullptr &&
                  caption != nullptr,
              "every authored id is reachable in the reported tree");

        if (headedRow != nullptr && plainRow != nullptr && headedLabel != nullptr &&
            caption != nullptr)
        {
            // The reading these tools exist for: a label's solved box lies inside the row that owns
            // it. A caller asking "is this text where its row is?" gets it from one call.
            const auto& rowRect = (*headedRow)["rect"];
            const auto& labelRect = (*headedLabel)["rect"];
            const f64 rowTop = rowRect.value("y", 0.0);
            const f64 rowBottom = rowTop + rowRect.value("h", 0.0);
            const f64 labelTop = labelRect.value("y", 0.0);
            const f64 labelBottom = labelTop + labelRect.value("h", 0.0);
            Check(labelTop >= rowTop && labelBottom <= rowBottom,
                  "a label's solved box lies within its row's");

            // And the heading is accounted for: the row under it starts below the caption's box,
            // which is what tells a caller a heading occupies layout rather than overlapping.
            const auto& captionRect = (*caption)["rect"];
            Check(rowTop >= captionRect.value("y", 0.0) + captionRect.value("h", 0.0),
                  "a headed row starts below the heading above it");
            Check(captionRect.value("h", 0.0) > 0.0, "the heading occupies layout height");

            Check((*headedRow).value("kind", std::string{}) == "Button",
                  "an element's kind is reported");
            Check((*headedLabel).value("text", std::string{}) == "IRON ORE",
                  "a text leaf reports the run it paints");
        }

        // Rooting the walk at an id reports that subtree and nothing above it.
        const Json subtree = Payload(CallTool(client, "gui.inspect", Json{{"id", "row-2"}}));
        Check(subtree["root"].value("id", std::string{}) == "row-2",
              "the walk roots at the named element");
        Check(FindNode(subtree["root"], "row-0") == nullptr,
              "a rooted walk reports nothing outside its subtree");

        // A depth floor says it elided rather than reporting a leaf, so a caller can tell the two
        // apart and widen the request.
        const Json shallow = Payload(CallTool(client, "gui.inspect", Json{{"depth", 0}}));
        Check(shallow["root"].contains("children_elided"),
              "a depth floor reports what it did not descend into");

        // An entity presenting no document is an error, not a silent fall-through to another one.
        const Json wrong =
            CallTool(client, "gui.inspect", Json{{"entity", static_cast<i64>(bare.Index)}});
        Check(wrong.is_object() && wrong.value("isError", false),
              "an entity with no document is an error");

        // So is an id no element carries.
        const Json missing = CallTool(client, "gui.inspect", Json{{"id", "no-such-element"}});
        Check(missing.is_object() && missing.value("isError", false),
              "an unknown element id is an error");
    }

    done.store(true);
    pump.join();
    server.reset();

    std::printf(g_Failures == 0 ? "mcp_gui: PASS\n" : "mcp_gui: FAIL (%d)\n", g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
