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
#include <poll.h>
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

    // The outcome of running a short-lived child to its natural exit: its drained stdout+stderr,
    // the real exit code, and whether it exited on its own. Exited == false means the child had
    // to be force-killed at the wait bound — a hang, reported as a test failure, never a masked 0.
    struct RunResult
    {
        std::string Output;
        int ExitCode = -1;
        bool Exited = false;
    };

    // Spawns args to completion, draining stdout+stderr and reading the real exit code. The wait
    // bound must exceed the client's own socket timeout so a slow-but-succeeding call exits on its
    // own before the force-kill; a force-kill leaves Exited false, the caller's hang signal.
    inline RunResult RunToCompletion(const std::vector<std::string>& args, int waitSeconds = 30)
    {
        RunResult result;
        Launched launched;
        if (!SpawnCaptured(args, launched))
        {
            return result;
        }

        char chunk[256];
        DWORD read = 0;
        while (ReadFile(launched.ReadPipe, chunk, sizeof(chunk), &read, nullptr) && read != 0)
        {
            result.Output.append(chunk, read);
        }

        const DWORD waited =
            WaitForSingleObject(launched.Process.hProcess, static_cast<DWORD>(waitSeconds) * 1000);
        if (waited == WAIT_OBJECT_0)
        {
            DWORD code = 0;
            GetExitCodeProcess(launched.Process.hProcess, &code);
            result.ExitCode = static_cast<int>(code);
            result.Exited = true;
        }
        else
        {
            TerminateProcess(launched.Process.hProcess, 0);
            WaitForSingleObject(launched.Process.hProcess, 5000);
        }
        CloseHandle(launched.Process.hProcess);
        CloseHandle(launched.Process.hThread);
        CloseHandle(launched.ReadPipe);
        return result;
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

    // The outcome of running a short-lived child to its natural exit: its drained stdout+stderr,
    // the real exit code, and whether it exited on its own. Exited == false means the child had
    // to be force-killed at the wait bound — a hang, reported as a test failure, never a masked 0.
    struct RunResult
    {
        std::string Output;
        int ExitCode = -1;
        bool Exited = false;
    };

    // Spawns args to completion, draining stdout+stderr and reading the real exit code via
    // WEXITSTATUS. The wait bound must exceed the client's own socket timeout so a slow-but-
    // succeeding call exits on its own before the SIGKILL; a force-kill leaves Exited false, the
    // caller's hang signal. The pipe read is deadline-polled so a hung child cannot block the
    // drain past the bound.
    inline RunResult RunToCompletion(const std::vector<std::string>& args, int waitSeconds = 30)
    {
        RunResult result;
        Launched launched;
        if (!SpawnCaptured(args, launched))
        {
            return result;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(waitSeconds);
        char chunk[256];
        while (std::chrono::steady_clock::now() < deadline)
        {
            pollfd fd{.fd = launched.ReadFd, .events = POLLIN, .revents = 0};
            const int ready = poll(&fd, 1, 100);
            if (ready < 0)
            {
                break;
            }
            if (ready == 0)
            {
                continue;
            }
            const ssize_t got = read(launched.ReadFd, chunk, sizeof(chunk));
            if (got <= 0)
            {
                break; // pipe closed — the child's stdout/stderr are done
            }
            result.Output.append(chunk, static_cast<std::size_t>(got));
        }

        int status = 0;
        // A closed pipe means the child is exiting; give the reap a brief bounded window before
        // force-killing, so a child that has already written all its output and is exiting cleanly
        // is reaped for its real code rather than raced to a SIGKILL.
        for (int attempt = 0; attempt < 50 && std::chrono::steady_clock::now() < deadline;
             ++attempt)
        {
            if (waitpid(launched.Pid, &status, WNOHANG) == launched.Pid)
            {
                result.ExitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                result.Exited = WIFEXITED(status);
                close(launched.ReadFd);
                return result;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        kill(launched.Pid, SIGKILL);
        waitpid(launched.Pid, &status, 0);
        close(launched.ReadFd);
        return result; // Exited stays false: the child had to be force-killed
    }

#endif
}
