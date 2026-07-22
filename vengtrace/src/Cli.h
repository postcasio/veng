#pragma once

#include <Veng/Veng.h>

#include <iosfwd>

// The vengtrace command-line surface, factored out of main so it drives in-process against captured
// streams — the same shape mcp_cli exercises RunClientCli with — so the arg grammar and the
// exit-code contract are asserted, not sampled.

namespace Veng::VengTrace
{
    /// @brief The documented process exit codes. Each nonzero value is a distinct, stable contract.
    enum class ExitCode : int
    {
        /// @brief Success. A truncated capture still converts and exits Ok.
        Ok = 0,
        /// @brief A command-line usage error (bad subcommand, missing or unknown option).
        Usage = 1,
        /// @brief The input file could not be read, or is not a veng capture.
        Unreadable = 2,
        /// @brief The capture's format version is one this tool does not recognize.
        UnknownVersion = 3,
        /// @brief The output file could not be written.
        WriteFailure = 4,
    };

    /// @brief Runs the vengtrace CLI over an argument vector, returning the process exit code.
    ///
    /// @param args  The arguments after the executable name; args[0] is the subcommand.
    /// @param out   The stdout sink (a success confirmation).
    /// @param err   The stderr sink (usage, errors, and the non-fatal truncation warning).
    /// @return The exit code, as an int, matching the ExitCode contract.
    [[nodiscard]] int RunVengtraceCli(const vector<string>& args, std::ostream& out,
                                      std::ostream& err);
}
