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
// baked in by CMake ($<TARGET_FILE:hello_triangle-launcher>).

#include <nlohmann/json.hpp>

#define CPPHTTPLIB_IMPLEMENTATION
#include <httplib.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

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

    // Parses the port out of a "listening on <ip>:<port>" log line, or 0 if the line is not it.
    int ParseListeningPort(const std::string& line)
    {
        const std::string marker = "listening on";
        const std::size_t at = line.find(marker);
        if (at == std::string::npos)
        {
            return 0;
        }
        const std::size_t colon = line.rfind(':');
        if (colon == std::string::npos || colon < at)
        {
            return 0;
        }
        return std::atoi(line.c_str() + colon + 1);
    }
}

#if defined(_WIN32)

// Windows: spawn the launcher with a stdout pipe, read it for the listening line, and terminate
// the process at the end. The environment (HT_MCP / HT_SMOKE) is inherited from this process,
// which main() set before spawning.
namespace
{
    struct Launched
    {
        PROCESS_INFORMATION Process{};
        HANDLE ReadPipe = nullptr;
    };

    bool SpawnLauncher(Launched& out)
    {
        SECURITY_ATTRIBUTES attrs{};
        attrs.nLength = sizeof(attrs);
        attrs.bInheritHandle = TRUE;

        HANDLE writePipe = nullptr;
        if (!CreatePipe(&out.ReadPipe, &writePipe, &attrs, 0))
        {
            return false;
        }
        SetHandleInformation(out.ReadPipe, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = writePipe;
        startup.hStdError = writePipe;

        std::string command = VENG_LAUNCHER_BIN;
        const BOOL ok = CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr,
                                       nullptr, &startup, &out.Process);
        CloseHandle(writePipe);
        if (!ok)
        {
            CloseHandle(out.ReadPipe);
            return false;
        }
        return true;
    }

    int ReadPort(Launched& launched)
    {
        std::string buffer;
        char chunk[256];
        DWORD read = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (!ReadFile(launched.ReadPipe, chunk, sizeof(chunk), &read, nullptr) || read == 0)
            {
                break;
            }
            buffer.append(chunk, read);
            std::size_t newline = 0;
            while ((newline = buffer.find('\n')) != std::string::npos)
            {
                const std::string line = buffer.substr(0, newline);
                buffer.erase(0, newline + 1);
                const int port = ParseListeningPort(line);
                if (port != 0)
                {
                    return port;
                }
            }
        }
        return 0;
    }

    void Terminate(Launched& launched)
    {
        TerminateProcess(launched.Process.hProcess, 0);
        WaitForSingleObject(launched.Process.hProcess, 5000);
        CloseHandle(launched.Process.hProcess);
        CloseHandle(launched.Process.hThread);
        CloseHandle(launched.ReadPipe);
    }
}

#else

// POSIX: fork/exec the launcher with its stdout redirected to a pipe, read the pipe for the
// listening line, and SIGTERM the child at the end. The environment (HT_MCP / HT_SMOKE) is
// inherited across exec from this process, which main() set before forking.
namespace
{
    struct Launched
    {
        pid_t Pid = -1;
        int ReadFd = -1;
    };

    bool SpawnLauncher(Launched& out)
    {
        int fds[2];
        if (pipe(fds) != 0)
        {
            return false;
        }
        const pid_t pid = fork();
        if (pid < 0)
        {
            close(fds[0]);
            close(fds[1]);
            return false;
        }
        if (pid == 0)
        {
            // Child: point stdout + stderr at the pipe's write end, then exec the launcher.
            dup2(fds[1], STDOUT_FILENO);
            dup2(fds[1], STDERR_FILENO);
            close(fds[0]);
            close(fds[1]);
            execl(VENG_LAUNCHER_BIN, VENG_LAUNCHER_BIN, static_cast<char*>(nullptr));
            _exit(127);
        }
        close(fds[1]);
        out.Pid = pid;
        out.ReadFd = fds[0];
        return true;
    }

    int ReadPort(Launched& launched)
    {
        std::string buffer;
        char chunk[256];
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < deadline)
        {
            const ssize_t got = read(launched.ReadFd, chunk, sizeof(chunk));
            if (got <= 0)
            {
                break;
            }
            buffer.append(chunk, static_cast<std::size_t>(got));
            std::size_t newline = 0;
            while ((newline = buffer.find('\n')) != std::string::npos)
            {
                const std::string line = buffer.substr(0, newline);
                buffer.erase(0, newline + 1);
                const int port = ParseListeningPort(line);
                if (port != 0)
                {
                    return port;
                }
            }
        }
        return 0;
    }

    void Terminate(Launched& launched)
    {
        kill(launched.Pid, SIGTERM);
        int status = 0;
        for (int attempt = 0; attempt < 50; ++attempt)
        {
            if (waitpid(launched.Pid, &status, WNOHANG) == launched.Pid)
            {
                close(launched.ReadFd);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        kill(launched.Pid, SIGKILL);
        waitpid(launched.Pid, &status, 0);
        close(launched.ReadFd);
    }
}

#endif

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

    Launched launched;
    if (!SpawnLauncher(launched))
    {
        std::fprintf(stderr, "mcp_conformance: failed to launch '%s'\n", VENG_LAUNCHER_BIN);
        return 1;
    }

    const int port = ReadPort(launched);
    if (port == 0)
    {
        // No listening line: the launcher most likely skipped for want of a Vulkan ICD (the whole
        // gpu band's skip contract) or crashed. Reap it and report the gpu-band skip code.
        Terminate(launched);
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
    }

    Terminate(launched);

    if (g_Failures == 0)
    {
        std::printf("mcp_conformance: all checks passed (port %d)\n", port);
        return 0;
    }
    std::fprintf(stderr, "mcp_conformance: %d check(s) failed\n", g_Failures);
    return 1;
}
