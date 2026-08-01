#pragma once

#include <Veng/Veng.h>

#include <ostream>
#include <span>

namespace Veng::Mcp
{
    /// @brief Parses an MCP-client argument vector, performs one call, and returns an exit code.
    ///
    /// The shared shell-facing driver behind both the editor's and a game launcher's
    /// `--connect` client: it parses the argument grammar, builds one McpClient from the
    /// parsed host/port, drives a single tool call (or the tools listing), and maps the
    /// outcome onto stdout / stderr and a process exit code. Both front ends are a thin
    /// call into this, so the grammar and the exit-code map live in one place.
    ///
    /// The grammar (args carry no program name — the front end strips argv[0]):
    /// - `--connect=<port>` targets 127.0.0.1:<port>; `--connect=<host>:<port>` an explicit
    ///   host (split on the last `:`; IPv6 literals are out of scope). Required — absent is a
    ///   usage error.
    /// - Exactly one of a positional `<tool-name>` or `--list` is present; both or neither is
    ///   a usage error. `--search <query>` filters `--list` by a case-insensitive substring
    ///   over name + description.
    /// - In tool mode, `key=value` pairs assemble the `arguments` object, each value parsed as
    ///   JSON with a string fallback; `--json '<object>'` supplies the object verbatim and is
    ///   mutually exclusive with any `key=value` (and must parse as a JSON object).
    /// - `--timeout <seconds>` raises the client's connect/read timeout from its 10 s default
    ///   (1 to 86400 seconds; anything else is a usage error). A tool that declares it runs
    ///   off the pump can run for longer than that default, and without this flag the caller
    ///   gives up before the result arrives. Valid in both tool and `--list` mode.
    /// - `--raw` prints the full `result` object instead of the concatenated text payload;
    ///   `--output <file>` writes an image content block (base64-decoded) to the given path,
    ///   overwriting it. A tool that returns an image content block (e.g. `render.screenshot`,
    ///   `editor.screenshot_panel`) REQUIRES `--output`: an image is never printed to stdout, so
    ///   calling such a tool without `--output` is a usage error. No base64 is ever emitted.
    ///
    /// The tool payload (or the tools listing) is written to @p out; any failure — connection
    /// refused, a JSON-RPC protocol error, or a tool result flagged isError — is written to
    /// @p err as one human-readable line prefixed with @p label. The exit codes are: 0 on
    /// success, 1 for a usage/argument error (including an image-returning tool called without
    /// `--output`), 2 for a connection failure (cannot reach the host), 3 for a JSON-RPC protocol
    /// error, 4 for a tool result flagged isError.
    /// @param args   The argument vector WITHOUT the program name (the front end strips argv[0]).
    /// @param out    Sink for the successful payload / tool listing.
    /// @param err    Sink for a single human-readable error line.
    /// @param label  The invoking exe's name, prefixed onto every error line (e.g. "veng-editor").
    /// @return The process exit code (0 on success; a distinct nonzero per failure class).
    VE_API int RunClientCli(std::span<const string> args, std::ostream& out, std::ostream& err,
                            string_view label);
}
