// Loopback proof for the veng::mcp request path, pumped and off-pump.
//
// Constructs an McpServer on Port = 0 with a `ping` tool, drives a background pump
// loop on this (render) thread, and from a client performs a real HTTP
// initialize -> tools/list (asserts `ping` present) -> tools/call ping (asserts the
// echo). It also feeds a malformed request body and asserts a clean JSON-RPC error —
// the containment check that a throw inside httplib's -fexceptions TU never unwinds
// out (no terminate). Pure logic + loopback, no GPU, so it runs in the default band.
//
// The same controlled pump then proves the off-pump declaration, which is testable here
// precisely because this harness owns when Pump() runs:
//   - A pumped tool resolves AT the pump and an off-pump tool resolves WITHOUT one —
//     driven by parking a pumped call with the pump stopped and answering an off-pump call
//     over its head, then resuming and watching the parked one land.
//   - The prologue seam: a declared prologue runs on the pumping thread, its result reaches
//     the handler (which runs on another thread), and a prologue that fails reports its error
//     with the handler never running.
//   - The two-in-flight bound: a third concurrent off-pump call is refused off-pump-busy, and
//     a second call to a tool already running is refused even with a slot free.
//   - Shutdown does not wait on a walk: ~McpServer returns promptly while an off-pump handler
//     polling its cancellation token is mid-run.
//
// The httplib client is the same vendored header compiled here (this TU builds with
// exceptions, like the rest of the test suite), so it needs no new dependency.

#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpServerInfo.h>
#include <Veng/Mcp/McpTool.h>

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

    // POST a JSON-RPC message to the server and return the parsed response body.
    Json Post(httplib::Client& client, const Json& message)
    {
        const httplib::Result res = client.Post("/", message.dump(), "application/json");
        if (!res)
        {
            return Json{{"error", "no response"}};
        }
        return Json::parse(res->body, nullptr, false);
    }

    // A tools/call message for the named tool.
    Json CallMessage(int id, const char* tool, Json arguments = Json::object())
    {
        return Json{{"jsonrpc", "2.0"},
                    {"id", id},
                    {"method", "tools/call"},
                    {"params", {{"name", tool}, {"arguments", std::move(arguments)}}}};
    }

    // A fresh loopback client — one per thread, since a Client owns a single socket.
    httplib::Client MakeClient(Veng::u16 port)
    {
        httplib::Client client("127.0.0.1", port);
        client.set_connection_timeout(5, 0);
        client.set_read_timeout(30, 0);
        return client;
    }

    // The concatenated text of a tools/call result's content blocks.
    std::string ResultText(const Json& response)
    {
        if (!response.contains("result") || !response["result"].contains("content"))
        {
            return {};
        }
        std::string text;
        for (const Json& block : response["result"]["content"])
        {
            text += block.value("text", std::string{});
        }
        return text;
    }

    // Spins until pred() holds or the limit expires; reports which.
    template <class Pred>
    bool WaitUntil(Pred pred, std::chrono::milliseconds limit)
    {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (pred())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return pred();
    }

    // Which thread a handler body ran on, published release/acquire so a reader on another
    // thread sees the id and not just the flag.
    struct ThreadMark
    {
        std::thread::id Id;
        std::atomic<bool> Set{false};

        void Mark()
        {
            Id = std::this_thread::get_id();
            Set.store(true, std::memory_order_release);
        }

        [[nodiscard]] bool WasMarked() const { return Set.load(std::memory_order_acquire); }
    };
}

int main()
{
    using namespace Veng;

    Mcp::McpServerInfo info;
    info.Port = 0;
    info.BindLoopbackOnly = true;

    // The loopback proof exercises only the ping tool and the auto-registered world tools with
    // no world (CurrentWorld returns null), which never touch Assets. Bind Assets through a
    // never-dereferenced pointer: an AssetManager needs a render Context this device-free test
    // has none of, and no code path here reads it.
    TypeRegistry registry;
    AssetManager* assets = nullptr;
    const Mcp::McpHost host{.Types = registry,
                            .Assets = *assets,
                            .CurrentWorld = [] { return static_cast<Scene*>(nullptr); },
                            .Viewport = {}};

    Unique<Mcp::McpServer> server = Mcp::McpServer::Create(info, host);

    Mcp::McpTool ping;
    ping.Name = "ping";
    ping.Description = "Echoes its message back.";
    ping.InputSchemaJson = R"({"type":"object","properties":{"message":{"type":"string"}}})";
    ping.Handler = [](string_view argsJson) -> Result<string>
    {
        const Json args = Json::parse(argsJson, nullptr, false);
        const std::string message =
            args.is_object() ? args.value("message", std::string{}) : std::string{};
        return Json{{"echo", message}}.dump();
    };
    server->RegisterTool(std::move(ping));

    // The pumped/off-pump pair: both echo, so the only difference under test is where they run.
    ThreadMark pumpedThread;
    Mcp::McpTool pumped;
    pumped.Name = "pumped.marker";
    pumped.Description = "Records the thread it ran on, at the pump point.";
    pumped.InputSchemaJson = R"({"type":"object"})";
    pumped.Handler = [&pumpedThread](string_view) -> Result<string>
    {
        pumpedThread.Mark();
        return std::string(R"({"ran":"pumped"})");
    };
    server->RegisterTool(std::move(pumped));

    ThreadMark offPumpThread;
    Mcp::McpTool offPump;
    offPump.Name = "off.marker";
    offPump.Description = "Records the thread it ran on, off the pump.";
    offPump.InputSchemaJson = R"({"type":"object"})";
    offPump.RunsOffPump = true;
    offPump.OffPumpHandler =
        [&offPumpThread](const Mcp::McpOffPumpRequest& request) -> Result<string>
    {
        offPumpThread.Mark();
        const Json args = Json::parse(request.Arguments, nullptr, false);
        return Json{
            {"ran", "off-pump"},
            {"echo", args.is_object() ? args.value("message", std::string{}) : std::string{}},
            {"snapshot", std::string(request.Snapshot)}}
            .dump();
    };
    server->RegisterTool(std::move(offPump));

    // The prologue seam: the snapshot is taken at the pump and read off it.
    ThreadMark prologueThread;
    ThreadMark snapshotHandlerThread;
    Mcp::McpTool snapshot;
    snapshot.Name = "off.snapshot";
    snapshot.Description = "Takes a pumped snapshot, then computes off the pump.";
    snapshot.InputSchemaJson = R"({"type":"object"})";
    snapshot.RunsOffPump = true;
    snapshot.PumpedPrologue = [&prologueThread]() -> Result<string>
    {
        prologueThread.Mark();
        return std::string(R"({"tick":7})");
    };
    snapshot.OffPumpHandler =
        [&snapshotHandlerThread](const Mcp::McpOffPumpRequest& request) -> Result<string>
    {
        snapshotHandlerThread.Mark();
        const Json taken = Json::parse(request.Snapshot, nullptr, false);
        return Json{{"tick", taken.is_object() ? taken.value("tick", 0) : -1}}.dump();
    };
    server->RegisterTool(std::move(snapshot));

    std::atomic<bool> badPrologueHandlerRan{false};
    Mcp::McpTool badPrologue;
    badPrologue.Name = "off.bad_prologue";
    badPrologue.Description = "Its prologue always fails.";
    badPrologue.InputSchemaJson = R"({"type":"object"})";
    badPrologue.RunsOffPump = true;
    badPrologue.PumpedPrologue = []() -> Result<string>
    { return std::unexpected(std::string("prologue refused")); };
    badPrologue.OffPumpHandler =
        [&badPrologueHandlerRan](const Mcp::McpOffPumpRequest&) -> Result<string>
    {
        badPrologueHandlerRan.store(true);
        return std::string("{}");
    };
    server->RegisterTool(std::move(badPrologue));

    // Three interchangeable gated tools for the concurrency bound: each parks in flight until
    // the test releases it, so the test decides how many handlers are running at once.
    std::atomic<int> gateInFlight{0};
    std::atomic<bool> gateReleased{false};
    const auto makeGate = [&gateInFlight, &gateReleased](const char* name)
    {
        Mcp::McpTool gate;
        gate.Name = name;
        gate.Description = "Parks off the pump until the test releases it.";
        gate.InputSchemaJson = R"({"type":"object"})";
        gate.RunsOffPump = true;
        gate.OffPumpHandler =
            [&gateInFlight, &gateReleased](const Mcp::McpOffPumpRequest& request) -> Result<string>
        {
            gateInFlight.fetch_add(1);
            while (!gateReleased.load() && !request.IsCancelled())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            gateInFlight.fetch_sub(1);
            return std::string(R"({"gate":"done"})");
        };
        return gate;
    };
    server->RegisterTool(makeGate("off.gate_a"));
    server->RegisterTool(makeGate("off.gate_b"));
    server->RegisterTool(makeGate("off.gate_c"));

    const u16 port = server->GetPort();
    Check(port != 0, "GetPort resolved an ephemeral port");

    // Drive Pump() on this thread until the client work is done — the render-thread role.
    // `pumping` is the switch a test flips to prove what does and does not need the pump.
    std::atomic<bool> done{false};
    std::atomic<bool> pumping{true};
    std::thread pump(
        [&]
        {
            while (!done.load())
            {
                if (pumping.load())
                {
                    server->Pump();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            server->Pump();
        });
    const std::thread::id pumpThreadId = pump.get_id();

    {
        httplib::Client client = MakeClient(port);

        const Json initialize = Post(client, Json{{"jsonrpc", "2.0"},
                                                  {"id", 1},
                                                  {"method", "initialize"},
                                                  {"params", Json::object()}});
        Check(initialize.contains("result"), "initialize returned a result");
        Check(initialize["result"].value("protocolVersion", std::string{}).size() > 0,
              "initialize reported a protocolVersion");
        Check(initialize["result"]["serverInfo"].value("name", std::string{}) == "veng",
              "initialize reported the server name");

        const Json list = Post(client, Json{{"jsonrpc", "2.0"},
                                            {"id", 2},
                                            {"method", "tools/list"},
                                            {"params", Json::object()}});
        Check(list.contains("result"), "tools/list returned a result");
        bool sawPing = false;
        bool sawOffPump = false;
        for (const Json& tool : list["result"]["tools"])
        {
            const std::string name = tool.value("name", std::string{});
            sawPing = sawPing || name == "ping";
            sawOffPump = sawOffPump || name == "off.marker";
        }
        Check(sawPing, "tools/list listed the ping tool");
        Check(sawOffPump, "tools/list lists an off-pump tool exactly like a pumped one");

        const Json call = Post(client, CallMessage(3, "ping", Json{{"message", "hello"}}));
        Check(call.contains("result"), "tools/call returned a result");
        Check(call["result"].value("isError", true) == false, "ping was not an error");
        const Json payload = Json::parse(ResultText(call), nullptr, false);
        Check(payload.value("echo", std::string{}) == "hello", "ping echoed its message");

        // A malformed body must come back a clean error, never a terminate — the
        // containment check for the -fexceptions vendor TU.
        const httplib::Result bad = client.Post("/", std::string("{ not json"), "application/json");
        Check(static_cast<bool>(bad), "malformed request got a response");
        if (bad)
        {
            const Json error = Json::parse(bad->body, nullptr, false);
            Check(error.contains("error"), "malformed request returned a JSON-RPC error");
        }

        // An unknown tool is a tools/call error result, not a protocol error.
        const Json unknown = Post(client, CallMessage(4, "does.not.exist"));
        Check(unknown.contains("result"), "unknown tool returned a result envelope");
        Check(unknown["result"].value("isError", false) == true, "unknown tool was an error");

        // ---- the pumped/off-pump split -------------------------------------------------
        // Stop pumping. A pumped call now parks; an off-pump call must answer over its head.
        pumping.store(false);

        std::atomic<bool> parkedReturned{false};
        Json parkedResponse;
        std::thread parkedCall(
            [&]
            {
                httplib::Client parked = MakeClient(port);
                parkedResponse = Post(parked, CallMessage(5, "pumped.marker"));
                parkedReturned.store(true);
            });

        // Let the parked request reach the server before answering over the top of it.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        Check(!parkedReturned.load(), "the pumped call is parked while nothing pumps");

        const Json offCall = Post(client, CallMessage(6, "off.marker", Json{{"message", "hi"}}));
        Check(offCall.contains("result") && offCall["result"].value("isError", true) == false,
              "the off-pump tool answered with no pump running");
        {
            const Json offPayload = Json::parse(ResultText(offCall), nullptr, false);
            Check(offPayload.value("ran", std::string{}) == "off-pump",
                  "the off-pump handler produced the result");
            Check(offPayload.value("echo", std::string{}) == "hi",
                  "the off-pump handler read its arguments");
            Check(offPayload.value("snapshot", std::string{}).empty(),
                  "a tool declaring no prologue receives an empty snapshot");
        }
        Check(!parkedReturned.load(),
              "the pumped call was still parked when the off-pump call had answered");

        pumping.store(true);
        parkedCall.join();
        Check(parkedReturned.load() && parkedResponse.contains("result") &&
                  parkedResponse["result"].value("isError", true) == false,
              "the pumped call resolved once the pump resumed");
        Check(pumpedThread.WasMarked() && pumpedThread.Id == pumpThreadId,
              "the pumped handler ran on the pumping thread");
        Check(offPumpThread.WasMarked() && offPumpThread.Id != pumpThreadId,
              "the off-pump handler ran on the network thread, not the pumping one");

        // ---- the prologue seam ----------------------------------------------------------
        const Json snapCall = Post(client, CallMessage(7, "off.snapshot"));
        Check(snapCall.contains("result") && snapCall["result"].value("isError", true) == false,
              "the prologue tool returned a result");
        {
            const Json snapPayload = Json::parse(ResultText(snapCall), nullptr, false);
            Check(snapPayload.value("tick", 0) == 7,
                  "the prologue's snapshot reached the off-pump handler");
        }
        Check(prologueThread.WasMarked() && prologueThread.Id == pumpThreadId,
              "the prologue ran at the pump point, on the pumping thread");
        Check(snapshotHandlerThread.WasMarked() && snapshotHandlerThread.Id != pumpThreadId,
              "its handler ran off the pumping thread");

        const Json badCall = Post(client, CallMessage(8, "off.bad_prologue"));
        Check(badCall.contains("result") && badCall["result"].value("isError", false) == true,
              "a failing prologue is reported as a tool error");
        Check(ResultText(badCall).find("prologue refused") != std::string::npos,
              "the prologue's own reason is what the client is told");
        Check(!badPrologueHandlerRan.load(), "a failing prologue does not run the handler");

        // ---- the two-in-flight bound -----------------------------------------------------
        gateReleased.store(false);
        std::thread gateA(
            [&]
            {
                httplib::Client c = MakeClient(port);
                Post(c, CallMessage(9, "off.gate_a"));
            });
        std::thread gateB(
            [&]
            {
                httplib::Client c = MakeClient(port);
                Post(c, CallMessage(10, "off.gate_b"));
            });
        Check(WaitUntil([&] { return gateInFlight.load() == 2; }, std::chrono::seconds(5)),
              "two off-pump handlers reached flight together");

        const Json third = Post(client, CallMessage(11, "off.gate_c"));
        Check(third.contains("result") && third["result"].value("isError", false) == true,
              "a third concurrent off-pump call is refused, not queued");
        Check(ResultText(third).find("off-pump busy") != std::string::npos,
              "the refusal is the distinct off-pump-busy error");
        Check(gateInFlight.load() == 2, "the refused call never entered the handler");

        gateReleased.store(true);
        gateA.join();
        gateB.join();
        Check(WaitUntil([&] { return gateInFlight.load() == 0; }, std::chrono::seconds(5)),
              "both gated handlers unwound");

        // No re-entry: a second call to a tool already running is refused with a slot free.
        gateReleased.store(false);
        std::thread soleGate(
            [&]
            {
                httplib::Client c = MakeClient(port);
                Post(c, CallMessage(12, "off.gate_a"));
            });
        Check(WaitUntil([&] { return gateInFlight.load() == 1; }, std::chrono::seconds(5)),
              "one off-pump handler is in flight");

        const Json reentrant = Post(client, CallMessage(13, "off.gate_a"));
        Check(reentrant.contains("result") && reentrant["result"].value("isError", false) == true,
              "a tool already running refuses a second concurrent call");
        Check(ResultText(reentrant).find("off-pump busy") != std::string::npos,
              "the re-entry refusal is the same off-pump-busy error");

        // ...and the slot it did not take really was free, for a different tool.
        std::thread otherGate(
            [&]
            {
                httplib::Client c = MakeClient(port);
                Post(c, CallMessage(14, "off.gate_c"));
            });
        Check(WaitUntil([&] { return gateInFlight.load() == 2; }, std::chrono::seconds(5)),
              "the second slot was free for another tool while the first tool held one");

        gateReleased.store(true);
        soleGate.join();
        otherGate.join();
    }

    done.store(true);
    pump.join();

    // ---- shutdown does not wait on a walk ----------------------------------------------
    // A second server whose off-pump handler would run for a minute: the destructor sets the
    // cancellation the handler polls, so teardown costs a poll interval, not the walk.
    {
        Mcp::McpServerInfo spinInfo;
        spinInfo.Port = 0;
        spinInfo.BindLoopbackOnly = true;
        Unique<Mcp::McpServer> spinServer = Mcp::McpServer::Create(spinInfo, host);

        std::atomic<bool> spinStarted{false};
        std::atomic<bool> spinSawCancel{false};
        Mcp::McpTool spin;
        spin.Name = "off.spin";
        spin.Description = "A long walk that polls its cancellation.";
        spin.InputSchemaJson = R"({"type":"object"})";
        spin.RunsOffPump = true;
        spin.OffPumpHandler =
            [&spinStarted, &spinSawCancel](const Mcp::McpOffPumpRequest& request) -> Result<string>
        {
            spinStarted.store(true);
            for (int step = 0; step < 30000; ++step) // a minute at this granularity
            {
                if (request.IsCancelled())
                {
                    spinSawCancel.store(true);
                    return std::string(R"({"cancelled":true})");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return std::string(R"({"cancelled":false})");
        };
        spinServer->RegisterTool(std::move(spin));

        const u16 spinPort = spinServer->GetPort();
        spinServer->Pump(); // the first pump is what starts the listener thread

        std::thread caller(
            [&]
            {
                httplib::Client c = MakeClient(spinPort);
                Post(c, CallMessage(15, "off.spin"));
            });
        Check(WaitUntil([&] { return spinStarted.load(); }, std::chrono::seconds(5)),
              "the long off-pump handler reached flight");

        const auto begin = std::chrono::steady_clock::now();
        spinServer.reset();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin);
        Check(elapsed < std::chrono::seconds(2),
              "~McpServer returned promptly with an off-pump handler mid-run");
        Check(spinSawCancel.load(), "the handler observed the cancellation the destructor set");
        caller.join();
    }

    if (g_Failures == 0)
    {
        std::printf("mcp_loopback: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "mcp_loopback: %d check(s) failed\n", g_Failures);
    return 1;
}
