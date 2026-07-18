// Headless proof for the veng::mcp input-injection tool (input.send).
//
// Constructs an McpServer with AllowMutations = true and an McpHost whose InjectInput closure
// folds each fabricated event into a headless Veng::Input (constructed with a null window, the
// documented neutral-state seam) via Input::ApplyEvent — the same seam the app's InputRouter uses.
// The pump loop calls Input::BeginFrame() before each Pump() so an injected key edge is visible in
// the same frame, mirroring the real run loop. Over loopback it drives input.send with:
//   - a key_down batch, asserting the key reads down through Input::IsKeyDown.
//   - a key_up, asserting it clears.
//   - a mouse_move, asserting Input::GetMousePosition tracks it.
//   - a mouse_down/up pair and a scroll, asserting the button + scroll state.
//   - a text run, asserting it types into a live Gui::Document's focused TextInput one codepoint at
//     a time (multi-byte codepoints and U+0008 backspace included) — the closure hands a KeyTyped
//     event to the document the way a viewport's GuiConsumer does, so the codepoint path from there
//     down is the engine's own.
//   - the shape-validation errors (empty batch, over-limit, an unknown key, an unknown type, an
//     empty / absent / over-limit text run, a malformed event) as whole-call isError results, and
//     confirms no event applied on a rejected batch (the validate-then-apply discipline).
// A second server with AllowMutations = false asserts input.send is absent from tools/list. A
// third with a null InjectInput asserts the tool reports injection unavailable. Pure logic +
// loopback, no GPU, so it runs in the default band.

#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpServerInfo.h>

#include <Veng/Event.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/DrawList.h>
#include <Veng/Gui/Element.h>
#include <Veng/Input.h>
#include <Veng/InputEvents.h>
#include <Veng/Reflection/TypeRegistry.h>

#include <nlohmann/json.hpp>

#define CPPHTTPLIB_IMPLEMENTATION
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
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

    Json CallToolResult(httplib::Client& client, const std::string& name, const Json& args)
    {
        const Json response = Post(client, Json{{"jsonrpc", "2.0"},
                                                {"id", g_Id++},
                                                {"method", "tools/call"},
                                                {"params", {{"name", name}, {"arguments", args}}}});
        return response.contains("result") ? response["result"] : Json(nullptr);
    }

    bool IsError(const Json& result)
    {
        return result.is_object() && result.value("isError", false);
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

    // A headless Input: null window reports the neutral all-zeros state until events are applied,
    // the documented test/injection contract. m_Input access is guarded so the injecting pump
    // thread and the asserting main thread never race — a real app touches Input on one thread.
    Input input(nullptr);
    std::mutex inputMutex;

    // A live Gui::Document with one focused TextInput — the text-entry oracle. In an app the
    // GuiConsumer routes a KeyTyped through the seat's viewports into each attached document; a
    // headless proof has no viewport, so the injection closure below hands the same event straight
    // to the document. Everything from the codepoint down (DispatchText -> the field's own edit) is
    // the engine's real text path, so a driven run exercises exactly what a typing user does.
    // A device-free measurer stands in for a resident font (eight pixels per codepoint), so the
    // field's painted caret is a checkable oracle for what a driven run put on screen.
    Gui::Document document;
    document.SetInteractive(true);
    document.SetTextMeasurer([](string_view text, const Gui::Style&, optional<f32>)
                             { return vec2(static_cast<f32>(text.size()) * 8.0f, 16.0f); });
    document.Root().Layout = Gui::Rect{.Min = {0.0f, 0.0f}, .Size = {200.0f, 200.0f}};
    Gui::Element& field = document.Add(document.Root(), Gui::ElementKind::TextInput);
    field.Layout = Gui::Rect{.Min = {0.0f, 0.0f}, .Size = {160.0f, 16.0f}};
    document.InitWidget(field);
    document.SetFocus(&field);

    // The x the field's caret paints at: the width of its value up to the edit position.
    const auto caretX = [&document]
    {
        Gui::DrawList painted;
        document.Build(painted);
        return painted.GetVertices().empty() ? -1.0f : painted.GetVertices()[0].Position.x;
    };

    // The input tools never touch Assets or a Scene; bind a never-dereferenced AssetManager, as the
    // other headless mcp tests do.
    AssetManager* assets = nullptr;
    const Mcp::McpHost host{
        .Types = registry,
        .Assets = *assets,
        .InjectInput =
            [&](Event& event)
        {
            const std::scoped_lock lock(inputMutex);
            input.ApplyEvent(event);
            if (event.GetEventType() == EventType::KeyTyped)
            {
                static_cast<void>(
                    document.DispatchText(static_cast<const KeyTypedEvent&>(event).GetCodepoint()));
            }
        },
    };

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
                {
                    // Roll the snapshot forward each pump so an injected edge is this frame's,
                    // exactly as the run loop calls BeginFrame before the event drain.
                    const std::scoped_lock lock(inputMutex);
                    input.BeginFrame();
                }
                server->Pump();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            server->Pump();
        });

    {
        httplib::Client client("127.0.0.1", port);
        client.set_connection_timeout(5, 0);
        client.set_read_timeout(10, 0);

        Check(ToolNames(client).count("input.send") == 1,
              "input.send registered under AllowMutations");

        // key_down: the key reads down.
        const Json down = CallToolResult(
            client, "input.send",
            Json{{"events", Json::array({Json{{"type", "key_down"}, {"key", "W"}}})}});
        Check(!IsError(down), "key_down batch succeeded");
        {
            const std::scoped_lock lock(inputMutex);
            Check(input.IsKeyDown(Key::W), "injected key_down set the key down");
        }

        // key_up: the key clears.
        const Json up =
            CallToolResult(client, "input.send",
                           Json{{"events", Json::array({Json{{"type", "key_up"}, {"key", "W"}}})}});
        Check(!IsError(up), "key_up batch succeeded");
        {
            const std::scoped_lock lock(inputMutex);
            Check(!input.IsKeyDown(Key::W), "injected key_up cleared the key");
        }

        // mouse_move: the position tracks. (Two moves so a non-zero position accumulates; the
        // first seeds m_HavePosition.)
        CallToolResult(
            client, "input.send",
            Json{{"events",
                  Json::array({Json{{"type", "mouse_move"}, {"x", 10.0f}, {"y", 20.0f}},
                               Json{{"type", "mouse_move"}, {"x", 42.0f}, {"y", 84.0f}}})}});
        {
            const std::scoped_lock lock(inputMutex);
            const vec2 pos = input.GetMousePosition();
            Check(std::abs(pos.x - 42.0f) < 1e-3f && std::abs(pos.y - 84.0f) < 1e-3f,
                  "injected mouse_move tracked the position");
        }

        // A mouse button down/up pair plus a scroll in one ordered batch.
        const Json mixed = CallToolResult(
            client, "input.send",
            Json{{"events", Json::array({Json{{"type", "mouse_down"}, {"button", "Right"}},
                                         Json{{"type", "scroll"}, {"dx", 0.0f}, {"dy", 3.0f}}})}});
        Check(!IsError(mixed), "mixed batch succeeded");
        {
            const std::scoped_lock lock(inputMutex);
            Check(input.IsMouseButtonDown(MouseButton::Right),
                  "injected mouse_down set the button");
            Check(std::abs(input.GetScrollDelta().y - 3.0f) < 1e-3f,
                  "injected scroll accumulated dy");
        }

        // text: the run types into whatever holds text focus, one character event per codepoint,
        // and the field owns the resulting value.
        const Json typed =
            CallToolResult(client, "input.send",
                           Json{{"events", Json::array({Json{{"type", "text"}, {"text", "Hi"}}})}});
        Check(!IsError(typed), "text batch succeeded");
        {
            const std::scoped_lock lock(inputMutex);
            Check(field.Text == "Hi", "an injected text run typed into the focused TextInput");
            // And the field paints what it now holds: the caret sits two codepoints in, so the
            // drive is observable in the draw list with no companion element mirroring the value.
            Check(std::abs(caretX() - 16.0f) < 1e-3f,
                  "the driven value moved the caret the field paints");
        }

        // A multi-byte codepoint arrives as one character event, not as its UTF-8 bytes, and a
        // U+0008 run backspaces — both prove the run rides the engine's own codepoint edit path.
        CallToolResult(client, "input.send",
                       Json{{"events", Json::array({Json{{"type", "text"}, {"text", "é"}},
                                                    Json{{"type", "text"}, {"text", "!"}}})}});
        {
            const std::scoped_lock lock(inputMutex);
            Check(field.Text == "Hié!", "a multi-byte codepoint typed as one character");
        }
        CallToolResult(client, "input.send",
                       Json{{"events", Json::array({Json{{"type", "text"}, {"text", "\b"}}})}});
        {
            const std::scoped_lock lock(inputMutex);
            Check(field.Text == "Hié", "an injected backspace deleted one codepoint");
        }

        // Shape-validation errors, each a whole-call isError.
        Check(IsError(CallToolResult(
                  client, "input.send",
                  Json{{"events", Json::array({Json{{"type", "text"}, {"text", ""}}})}})),
              "an empty text run is a whole-call error");

        Check(IsError(CallToolResult(
                  client, "input.send",
                  Json{{"events", Json::array({Json{{"type", "text"}, {"key", "A"}}})}})),
              "a text event with no 'text' string is a whole-call error");

        Check(IsError(CallToolResult(
                  client, "input.send",
                  Json{{"events",
                        Json::array({Json{{"type", "text"}, {"text", std::string(257, 'x')}}})}})),
              "an over-limit text run is a whole-call error");

        Check(IsError(CallToolResult(client, "input.send", Json{{"events", Json::array()}})),
              "an empty events array is a whole-call error");

        Json overLimit = Json::array();
        for (int i = 0; i < 65; ++i)
        {
            overLimit.push_back(Json{{"type", "key_down"}, {"key", "A"}});
        }
        Check(IsError(CallToolResult(client, "input.send", Json{{"events", overLimit}})),
              "an over-limit batch is a whole-call error");

        Check(IsError(CallToolResult(
                  client, "input.send",
                  Json{{"events", Json::array({Json{{"type", "key_down"}, {"key", "NotAKey"}}})}})),
              "an unknown key name is a whole-call error");

        Check(IsError(CallToolResult(client, "input.send",
                                     Json{{"events", Json::array({Json{{"type", "teleport"}}})}})),
              "an unknown event type is a whole-call error");

        // A rejected batch applies NO event: a good event followed by a malformed one must leave
        // the key untouched (validate-then-apply).
        CallToolResult(client, "input.send",
                       Json{{"events", Json::array({Json{{"type", "key_down"}, {"key", "Q"}},
                                                    Json{{"type", "key_down"}}})}});
        {
            const std::scoped_lock lock(inputMutex);
            Check(!input.IsKeyDown(Key::Q),
                  "a batch rejected on a later malformed event applied nothing");
        }
    }

    done.store(true);
    pump.join();

    // A read-only server never registers input.send.
    {
        Mcp::McpServerInfo readInfo;
        readInfo.Port = 0;
        readInfo.AllowMutations = false;
        Unique<Mcp::McpServer> readServer = Mcp::McpServer::Create(readInfo, host);
        const u16 readPort = readServer->GetPort();

        std::atomic<bool> readDone{false};
        std::thread readPump(
            [&]
            {
                while (!readDone.load())
                {
                    readServer->Pump();
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                readServer->Pump();
            });

        {
            httplib::Client client("127.0.0.1", readPort);
            client.set_connection_timeout(5, 0);
            Check(ToolNames(client).count("input.send") == 0,
                  "input.send absent from a read-only server");
        }
        readDone.store(true);
        readPump.join();
    }

    // A host with no InjectInput reports injection unavailable rather than no-op silently.
    {
        const Mcp::McpHost noInject{.Types = registry, .Assets = *assets};
        Mcp::McpServerInfo injInfo;
        injInfo.Port = 0;
        injInfo.AllowMutations = true;
        Unique<Mcp::McpServer> injServer = Mcp::McpServer::Create(injInfo, noInject);
        const u16 injPort = injServer->GetPort();

        std::atomic<bool> injDone{false};
        std::thread injPump(
            [&]
            {
                while (!injDone.load())
                {
                    injServer->Pump();
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                injServer->Pump();
            });

        {
            httplib::Client client("127.0.0.1", injPort);
            client.set_connection_timeout(5, 0);
            client.set_read_timeout(10, 0);
            const Json result = CallToolResult(
                client, "input.send",
                Json{{"events", Json::array({Json{{"type", "key_down"}, {"key", "W"}}})}});
            Check(IsError(result), "input.send with no InjectInput host is a tool error");
        }
        injDone.store(true);
        injPump.join();
    }

    if (g_Failures == 0)
    {
        std::printf("mcp_input: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "mcp_input: %d check(s) failed\n", g_Failures);
    return 1;
}
