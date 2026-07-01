// Spawns a child process with its stdout+stderr redirected to a pipe, scans the pipe for the
// MCP server's "listening on <ip>:<port>" readiness line, and terminates the child — the process
// scaffolding shared by the MCP conformance tests (mcp_conformance, editor_mcp_conformance).
// Header-only and veng-free: the conformance tests are standalone exes that link no engine
// library, so this stays plain std + platform calls.

#pragma once

#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#endif

namespace VengTest
{
    // Parses the port out of a "listening on <ip>:<port>" log line, or 0 if the line is not it.
    inline int ParseListeningPort(const std::string& line)
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

#if defined(_WIN32)

    struct Launched
    {
        PROCESS_INFORMATION Process{};
        HANDLE ReadPipe = nullptr;
    };

    // Spawns args[0] with the remaining args on its command line, stdout+stderr piped. The
    // environment is inherited from this process, so env gates are set by the caller before this.
    inline bool SpawnCaptured(const std::vector<std::string>& args, Launched& out)
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

        std::string command;
        for (const std::string& arg : args)
        {
            if (!command.empty())
            {
                command += ' ';
            }
            command += '"';
            command += arg;
            command += '"';
        }
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

    // Reads the child's output until the listening line appears; 0 on timeout or pipe close.
    inline int ReadPort(Launched& launched)
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

    inline void Terminate(Launched& launched)
    {
        TerminateProcess(launched.Process.hProcess, 0);
        WaitForSingleObject(launched.Process.hProcess, 5000);
        CloseHandle(launched.Process.hProcess);
        CloseHandle(launched.Process.hThread);
        CloseHandle(launched.ReadPipe);
    }

    // True while the child has not exited — the liveness probe the crash-regression checks use.
    inline bool IsRunning(const Launched& launched)
    {
        return WaitForSingleObject(launched.Process.hProcess, 0) == WAIT_TIMEOUT;
    }

#else

    struct Launched
    {
        pid_t Pid = -1;
        int ReadFd = -1;
    };

    // Forks and execs args[0] with the remaining args, stdout+stderr piped. The environment is
    // inherited across exec, so env gates are set by the caller before this.
    inline bool SpawnCaptured(const std::vector<std::string>& args, Launched& out)
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
            // Child: point stdout + stderr at the pipe's write end, then exec.
            dup2(fds[1], STDOUT_FILENO);
            dup2(fds[1], STDERR_FILENO);
            close(fds[0]);
            close(fds[1]);
            std::vector<char*> argv;
            argv.reserve(args.size() + 1);
            for (const std::string& arg : args)
            {
                argv.push_back(const_cast<char*>(arg.c_str()));
            }
            argv.push_back(nullptr);
            execv(argv[0], argv.data());
            _exit(127);
        }
        close(fds[1]);
        out.Pid = pid;
        out.ReadFd = fds[0];
        return true;
    }

    // Reads the child's output until the listening line appears; 0 on timeout or pipe close.
    inline int ReadPort(Launched& launched)
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

    inline void Terminate(Launched& launched)
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

    // True while the child has not exited — the liveness probe the crash-regression checks use.
    inline bool IsRunning(const Launched& launched)
    {
        int status = 0;
        return waitpid(launched.Pid, &status, WNOHANG) == 0;
    }

#endif
}
