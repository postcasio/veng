#pragma once

namespace Veng
{
    /// @brief Installs a process-wide handler that prints a symbolized backtrace on a fatal crash.
    ///
    /// On Windows an unhandled structured exception (an access violation, a fail-fast) otherwise
    /// kills the process with nothing on the log — the event log records only the faulting module
    /// and offset. The handler prints the exception code, the faulting address, and a stack walk
    /// to stderr, resolving symbols and source lines through dbghelp where PDBs are present, then
    /// lets the default handling continue so Windows Error Reporting still records the crash.
    /// On other platforms this is a no-op. Idempotent; Application::Run installs it.
    void InstallCrashReporter();
}
