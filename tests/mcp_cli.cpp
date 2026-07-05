// In-process proof for the shared client driver (RunClientCli).
//
// Stands an McpServer up in-process on Port = 0 with three tools — `ping` (echoes its
// message), `err` (returns a located error, surfaced as an isError tool result), and
// `echo_args` (returns its whole arguments object verbatim, so a test can assert the JSON
// typing of key=value pairs) — drives a background pump loop on this (render) thread, and
// calls RunClientCli with argument vectors, a fixed test label, and std::ostringstream
// sinks, asserting the returned exit code plus the captured stdout/stderr:
//   - --connect=<port> --list                  -> 0, listing carries the registered tools.
//   - --connect=<port> --list --search <substr> -> 0, listing narrows to the match.
//   - --connect=<port> ping message=hi          -> 0, payload on out.
//   - --connect=<port> err                      -> 4, label-prefixed error on err, out empty.
//   - --connect=<port> does.not.exist           -> 3, protocol-error path (unknown tool is a
//                                                  JSON-RPC error over tools/call? no — the
//                                                  server reports it as an isError result, so
//                                                  it is the tool-error row: exit 4).
//   - --connect=<deadport> ping                 -> 2 within the timeout.
//   - missing --connect / both --list and a tool / --json with a stray key=value -> 1 usage.
//   - echo_args limit=2 name=foo                 -> limit is a JSON number, name a JSON string.
//   - --json '{"a":1}' and a --json scalar        -> object forwarded / usage error.
//   - make_image (an image content-block tool)   -> without --output a usage error (exit 1, no
//                                                  base64 to any sink); with --output the decoded
//                                                  image bytes land in the file.
//
// Pure logic + loopback, no GPU, so it runs in the default band.

#include <Veng/Mcp/McpClientCli.h>
#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpServerInfo.h>
#include <Veng/Mcp/McpTool.h>

#include <Veng/Reflection/TypeRegistry.h>

#include <nlohmann/json.hpp>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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

    // Convenience: run RunClientCli with a fixed label and captured sinks.
    struct RunResult
    {
        int Code = 0;
        std::string Out;
        std::string Err;
    };

    RunResult Run(const std::vector<Veng::string>& args)
    {
        std::ostringstream out;
        std::ostringstream err;
        const int code = Veng::Mcp::RunClientCli(args, out, err, "veng-test");
        return RunResult{.Code = code, .Out = out.str(), .Err = err.str()};
    }
}

int main()
{
    using namespace Veng;

    Mcp::McpServerInfo info;
    info.Port = 0;
    info.BindLoopbackOnly = true;

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

    Mcp::McpTool err;
    err.Name = "err";
    err.Description = "Always fails with a located error.";
    err.InputSchemaJson = R"({"type":"object"})";
    err.Handler = [](string_view) -> Result<string>
    { return std::unexpected(std::string("deliberate tool failure")); };
    server->RegisterTool(std::move(err));

    Mcp::McpTool echoArgs;
    echoArgs.Name = "echo_args";
    echoArgs.Description = "Returns its arguments object verbatim.";
    echoArgs.InputSchemaJson = R"({"type":"object"})";
    echoArgs.Handler = [](string_view argsJson) -> Result<string>
    {
        const Json args = Json::parse(argsJson, nullptr, false);
        return (args.is_object() ? args : Json::object()).dump();
    };
    server->RegisterTool(std::move(echoArgs));

    // A content-block tool returning an image block (plus a text block), the shape
    // render.screenshot produces — exercises the CLI's --output / no-output image path without a
    // GPU. "AAECAw==" is base64 for the four bytes 0x00 0x01 0x02 0x03.
    Mcp::McpTool image;
    image.Name = "make_image";
    image.Description = "Returns a tiny image content block (requires --output on the CLI).";
    image.InputSchemaJson = R"({"type":"object"})";
    image.ReturnsContentBlocks = true;
    image.Handler = [](string_view) -> Result<string>
    {
        return Json::array(
                   {Json{{"type", "image"}, {"data", "AAECAw=="}, {"mimeType", "image/png"}},
                    Json{{"type", "text"}, {"text", R"({"note":"hi"})"}}})
            .dump();
    };
    server->RegisterTool(std::move(image));

    const u16 port = server->GetPort();
    Check(port != 0, "GetPort resolved an ephemeral port");
    const std::string portStr = std::to_string(port);

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
        // --list: exit 0, listing carries the registered tools.
        const RunResult list = Run({"--connect=" + portStr, "--list"});
        Check(list.Code == 0, "--list exited 0");
        Check(list.Out.find("ping") != std::string::npos, "--list output named ping");
        Check(list.Out.find("echo_args") != std::string::npos, "--list output named echo_args");
        Check(list.Err.empty(), "--list wrote nothing to err");

        // --list --search narrows the listing. "_args" matches echo_args's name only — note
        // ping's description ("Echoes its message back.") would match a bare "echo".
        const RunResult search = Run({"--connect=" + portStr, "--list", "--search", "_args"});
        Check(search.Code == 0, "--search exited 0");
        Check(search.Out.find("echo_args") != std::string::npos, "--search kept the match");
        Check(search.Out.find("ping") == std::string::npos, "--search dropped the non-match");

        // Tool call OK: payload on out.
        const RunResult ok = Run({"--connect=" + portStr, "ping", "message=hi"});
        Check(ok.Code == 0, "ping exited 0");
        Check(ok.Err.empty(), "ping wrote nothing to err");
        {
            const Json payload = Json::parse(ok.Out, nullptr, false);
            Check(payload.is_object() && payload.value("echo", std::string{}) == "hi",
                  "ping payload echoed the message on out");
        }

        // Tool result isError: exit 4, label-prefixed line on err, out empty.
        const RunResult toolErr = Run({"--connect=" + portStr, "err"});
        Check(toolErr.Code == 4, "err tool exited 4");
        Check(toolErr.Out.empty(), "err tool wrote nothing to out");
        Check(toolErr.Err.rfind("veng-test: err:", 0) == 0, "err tool line is label+tool prefixed");

        // Unknown tool: the server reports it as an isError tool result, so it is the tool-error
        // row (exit 4), matching mcp_client's observation.
        const RunResult unknown = Run({"--connect=" + portStr, "does.not.exist"});
        Check(unknown.Code == 4, "unknown tool exited 4 (isError tool result)");
        Check(unknown.Err.rfind("veng-test:", 0) == 0, "unknown tool line is label-prefixed");

        // key=value typing: limit reaches the tool as a JSON number, name as a string.
        const RunResult typed = Run({"--connect=" + portStr, "echo_args", "limit=2", "name=foo"});
        Check(typed.Code == 0, "echo_args exited 0");
        {
            const Json payload = Json::parse(typed.Out, nullptr, false);
            Check(payload.is_object(), "echo_args returned an object");
            Check(payload["limit"].is_number(), "limit=2 typed as a JSON number");
            Check(payload.value("limit", 0) == 2, "limit=2 carried the value 2");
            Check(payload["name"].is_string(), "name=foo typed as a JSON string");
            Check(payload.value("name", std::string{}) == "foo", "name=foo carried \"foo\"");
        }

        // --json supplies the whole object verbatim.
        const RunResult jsonArgs =
            Run({"--connect=" + portStr, "echo_args", "--json", R"({"a":1})"});
        Check(jsonArgs.Code == 0, "echo_args --json exited 0");
        {
            const Json payload = Json::parse(jsonArgs.Out, nullptr, false);
            Check(payload.is_object() && payload.value("a", 0) == 1, "--json forwarded the object");
        }

        // --raw prints the full result object.
        const RunResult raw = Run({"--connect=" + portStr, "--raw", "ping", "message=hi"});
        Check(raw.Code == 0, "--raw ping exited 0");
        Check(raw.Out.find("isError") != std::string::npos,
              "--raw output carried the result envelope");

        // An image-returning tool without --output is a usage error (exit 1): an image is never
        // printed to stdout, and no base64 leaks to either sink.
        const RunResult noOutput = Run({"--connect=" + portStr, "make_image"});
        Check(noOutput.Code == 1, "an image tool without --output is a usage error");
        Check(noOutput.Out.empty(), "an image tool without --output wrote nothing to out");
        Check(noOutput.Err.find("--output") != std::string::npos,
              "the error names --output as the fix");
        Check(noOutput.Out.find("AAECAw==") == std::string::npos &&
                  noOutput.Err.find("AAECAw==") == std::string::npos,
              "no base64 leaked to either sink");

        // With --output the decoded image bytes are written to the file, out is a confirmation.
        // A per-pid name in the CWD (the build dir) avoids a collision under parallel ctest and
        // the deprecated tmpnam.
        const std::string imagePath = "mcp_cli_image_" + std::to_string(::getpid()) + ".png";
        const RunResult output = Run({"--connect=" + portStr, "make_image", "--output", imagePath});
        Check(output.Code == 0, "an image tool with --output exited 0");
        Check(output.Out.find("AAECAw==") == std::string::npos, "the confirmation is not base64");
        {
            std::ifstream file(imagePath, std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());
            Check(bytes.size() == 4, "--output wrote the 4 decoded image bytes");
            Check(bytes.size() == 4 && bytes[0] == '\x00' && bytes[1] == '\x01' &&
                      bytes[2] == '\x02' && bytes[3] == '\x03',
                  "--output wrote the correct decoded bytes");
        }
        std::remove(imagePath.c_str());

        // Dead port: exit 2 within the timeout, connection line on err.
        const RunResult dead = Run({"--connect=1", "ping"});
        Check(dead.Code == 2, "dead port exited 2 (cannot reach)");
        Check(dead.Err.find("cannot reach 127.0.0.1:1") != std::string::npos,
              "dead-port line named the host:port");
    }

    // Usage errors (exit 1) need no server round-trip.
    {
        const RunResult noConnect = Run({"ping"});
        Check(noConnect.Code == 1, "missing --connect is a usage error");
        Check(noConnect.Err.rfind("veng-test: usage:", 0) == 0, "usage line is label-prefixed");

        const RunResult both = Run({"--connect=" + portStr, "--list", "ping"});
        Check(both.Code == 1, "both --list and a tool is a usage error");

        const RunResult neither = Run({"--connect=" + portStr});
        Check(neither.Code == 1, "neither --list nor a tool is a usage error");

        const RunResult jsonAndKv =
            Run({"--connect=" + portStr, "echo_args", "--json", "{}", "a=1"});
        Check(jsonAndKv.Code == 1, "--json with a stray key=value is a usage error");

        const RunResult jsonScalar = Run({"--connect=" + portStr, "echo_args", "--json", "5"});
        Check(jsonScalar.Code == 1, "--json with a scalar is a usage error");

        const RunResult searchNoList = Run({"--connect=" + portStr, "ping", "--search", "x"});
        Check(searchNoList.Code == 1, "--search without --list is a usage error");

        const RunResult badPort = Run({"--connect=notaport", "--list"});
        Check(badPort.Code == 1, "a non-numeric --connect port is a usage error");
    }

    done.store(true);
    pump.join();

    if (g_Failures == 0)
    {
        std::printf("mcp_cli: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "mcp_cli: %d check(s) failed\n", g_Failures);
    return 1;
}
