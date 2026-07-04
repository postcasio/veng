#pragma once

#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace Veng::TestSupport
{
    // The current process id, used to isolate each test process's scratch tree.
    inline unsigned long long CurrentProcessId()
    {
#if defined(_WIN32)
        return static_cast<unsigned long long>(_getpid());
#else
        return static_cast<unsigned long long>(::getpid());
#endif
    }

    /// @brief A process-unique scratch directory under the system temp tree.
    ///
    /// Created on first call and reused for the process's lifetime; keyed on the process id, so two
    /// concurrent test processes never share it. `ctest` discovers each doctest `TEST_CASE` as its
    /// own entry and runs them in parallel under `-j`, so the *same* test binary is invoked many
    /// times concurrently — a fixed temp path (`temp_directory_path() / "x.vengpack"`) then collides
    /// across those invocations. Routing every test's scratch path through this per-process directory
    /// removes the collision without giving every call site a bespoke unique name. Like the prior
    /// direct `temp_directory_path()` use, scratch files are left in the OS temp tree (transient).
    /// @return The process's scratch directory (guaranteed to exist).
    inline const std::filesystem::path& TempDir()
    {
        static const std::filesystem::path dir = []
        {
            const std::filesystem::path base = std::filesystem::temp_directory_path() /
                                               ("veng_test_" + std::to_string(CurrentProcessId()));
            std::error_code ec;
            std::filesystem::create_directories(base, ec);
            return base;
        }();
        return dir;
    }
}
