#include <Veng/Mcp/McpClientCli.h>

#include <Veng/Mcp/McpClient.h>
#include <Veng/Mcp/McpClientInfo.h>

#include <nlohmann/json.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

// The client driver is the CALLER of nlohmann, the same position as McpClient.cpp: a throw
// from an nlohmann operation must not propagate out of this TU into the -fno-exceptions front
// end. Every nlohmann read here is non-throwing (parse with the non-throwing overload, dump
// with the replace handler), so nothing throws — the TU is compiled -fexceptions only because
// nlohmann's headers name throw internally. See mcp/CMakeLists.txt.
namespace Veng::Mcp
{
    using Json = nlohmann::json;

    namespace
    {
        /// @brief The exit codes RunClientCli returns, one per outcome class.
        enum class ExitCode : int
        {
            Ok = 0,
            Usage = 1,
            Connect = 2,
            Rpc = 3,
            ToolError = 4,
        };

        /// @brief Serializes JSON with the non-throwing UTF-8 handler (no throw escapes).
        string DumpSafe(const Json& value)
        {
            return value.dump(-1, ' ', false, Json::error_handler_t::replace);
        }

        /// @brief Pretty-prints JSON with the non-throwing UTF-8 handler.
        string DumpPretty(const Json& value)
        {
            return value.dump(2, ' ', false, Json::error_handler_t::replace);
        }

        /// @brief A usage error: writes "<label>: usage: <detail>" and returns Usage.
        int UsageError(std::ostream& err, string_view label, string_view detail)
        {
            err << fmt::format("{}: usage: {}\n", label, detail);
            return static_cast<int>(ExitCode::Usage);
        }

        /// @brief The parsed connection target (host + port).
        struct Target
        {
            string Host;
            u16 Port = 0;
        };

        /// @brief Parses a --connect value ("<port>" or "<host>:<port>") into a Target.
        ///
        /// The value is split on the last ':' so a "host:port" form is unambiguous; a bare
        /// integer is 127.0.0.1:<port>. A malformed or out-of-range value yields nullopt.
        optional<Target> ParseConnect(string_view value)
        {
            if (value.empty())
            {
                return std::nullopt;
            }

            const auto parsePort = [](string_view text) -> optional<u16>
            {
                if (text.empty())
                {
                    return std::nullopt;
                }
                unsigned long parsed = 0;
                for (const char c : text)
                {
                    if (std::isdigit(static_cast<unsigned char>(c)) == 0)
                    {
                        return std::nullopt;
                    }
                    parsed = parsed * 10 + static_cast<unsigned long>(c - '0');
                    if (parsed > 65535)
                    {
                        return std::nullopt;
                    }
                }
                return static_cast<u16>(parsed);
            };

            const auto colon = value.rfind(':');
            if (colon == string_view::npos)
            {
                const optional<u16> port = parsePort(value);
                if (!port)
                {
                    return std::nullopt;
                }
                return Target{.Host = "127.0.0.1", .Port = *port};
            }

            const string_view host = value.substr(0, colon);
            const optional<u16> port = parsePort(value.substr(colon + 1));
            if (host.empty() || !port)
            {
                return std::nullopt;
            }
            return Target{.Host = string(host), .Port = *port};
        }

        /// @brief Decodes standard (RFC 4648, padded) base64 into raw bytes.
        ///
        /// Matches the encoder the server uses for an image content block. Returns nullopt on
        /// any invalid character or truncated group, so a bad block never writes a file.
        optional<string> Base64Decode(string_view input)
        {
            const auto sextet = [](char c) -> int
            {
                if (c >= 'A' && c <= 'Z')
                {
                    return c - 'A';
                }
                if (c >= 'a' && c <= 'z')
                {
                    return c - 'a' + 26;
                }
                if (c >= '0' && c <= '9')
                {
                    return c - '0' + 52;
                }
                if (c == '+')
                {
                    return 62;
                }
                if (c == '/')
                {
                    return 63;
                }
                return -1;
            };

            string out;
            out.reserve(input.size() / 4 * 3);

            int accumulator = 0;
            int bits = 0;
            for (const char c : input)
            {
                if (c == '=')
                {
                    break;
                }
                const int value = sextet(c);
                if (value < 0)
                {
                    return std::nullopt;
                }
                accumulator = (accumulator << 6) | value;
                bits += 6;
                if (bits >= 8)
                {
                    bits -= 8;
                    out.push_back(static_cast<char>((accumulator >> bits) & 0xFF));
                }
            }
            return out;
        }

        /// @brief Lowercases an ASCII string for case-insensitive --search matching.
        string ToLowerAscii(string_view text)
        {
            string out(text);
            std::ranges::transform(out, out.begin(), [](unsigned char c)
                                   { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        /// @brief Runs the --list / --search path against a live client.
        int RunList(McpClient& client, std::ostream& out, std::ostream& err, string_view label,
                    const Target& target, const optional<string>& search, bool raw)
        {
            const Result<vector<McpToolDesc>> tools = client.ListTools();
            if (!tools)
            {
                // ListTools folds a transport failure and a JSON-RPC error into one Result
                // error; the two are distinguished by the message shape McpClient produces
                // ("no response from server" / a "failed to create" transport line vs. an
                // "rpc error …" protocol line).
                const string& detail = tools.error();
                if (detail.rfind("rpc error", 0) == 0)
                {
                    err << fmt::format("{}: rpc error: {}\n", label, detail);
                    return static_cast<int>(ExitCode::Rpc);
                }
                err << fmt::format("{}: cannot reach {}:{}: {}\n", label, target.Host, target.Port,
                                   detail);
                return static_cast<int>(ExitCode::Connect);
            }

            const optional<string> needle =
                search ? optional<string>(ToLowerAscii(*search)) : std::nullopt;

            for (const McpToolDesc& tool : *tools)
            {
                if (needle)
                {
                    const string haystack = ToLowerAscii(tool.Name + " " + tool.Description);
                    if (haystack.find(*needle) == string::npos)
                    {
                        continue;
                    }
                }

                if (raw)
                {
                    const Json schema = Json::parse(tool.InputSchema, nullptr, false);
                    Json entry = Json::object();
                    entry["name"] = tool.Name;
                    entry["description"] = tool.Description;
                    entry["inputSchema"] = schema.is_discarded() ? Json(nullptr) : schema;
                    out << DumpPretty(entry) << "\n";
                }
                else
                {
                    out << fmt::format("{} — {}\n", tool.Name, tool.Description);
                }
            }
            return static_cast<int>(ExitCode::Ok);
        }

        /// @brief Runs the tool-call path against a live client.
        int RunCall(McpClient& client, std::ostream& out, std::ostream& err, string_view label,
                    const Target& target, string_view tool, const string& argumentsJson, bool raw,
                    const optional<string>& outputPath)
        {
            const Result<McpCallResult> call = client.CallTool(tool, argumentsJson);
            if (!call)
            {
                const string& detail = call.error();
                if (detail.rfind("rpc error", 0) == 0)
                {
                    err << fmt::format("{}: rpc error: {}\n", label, detail);
                    return static_cast<int>(ExitCode::Rpc);
                }
                err << fmt::format("{}: cannot reach {}:{}: {}\n", label, target.Host, target.Port,
                                   detail);
                return static_cast<int>(ExitCode::Connect);
            }

            if (call->IsError)
            {
                // The reported error already names the tool — the server attaches the name
                // where it dispatches, so the client adds only its own label. The client-side
                // failures below are not the server's to name, so they name the tool themselves.
                err << fmt::format("{}: {}\n", label, call->Content);
                return static_cast<int>(ExitCode::ToolError);
            }

            if (raw)
            {
                // Reconstruct the result object shape the caller asked to see verbatim: the
                // concatenated text, the isError flag, and any image block descriptor.
                Json result = Json::object();
                result["isError"] = call->IsError;
                if (!call->Content.empty())
                {
                    const Json parsed = Json::parse(call->Content, nullptr, false);
                    result["content"] = parsed.is_discarded() ? Json(call->Content) : parsed;
                }
                if (call->Image)
                {
                    result["image"] = Json{{"mimeType", call->Image->MimeType},
                                           {"bytes", call->Image->Base64Data.size()}};
                }
                out << DumpPretty(result) << "\n";
                return static_cast<int>(ExitCode::Ok);
            }

            if (call->Image)
            {
                // An image content block is binary; it is never written to stdout (base64 or
                // otherwise). --output is the only sink for it, so its absence is a usage error.
                if (!outputPath)
                {
                    err << fmt::format(
                        "{}: {}: returns an image content block — pass --output <file> to write "
                        "it (an image is never printed to stdout)\n",
                        label, tool);
                    return static_cast<int>(ExitCode::Usage);
                }

                const optional<string> bytes = Base64Decode(call->Image->Base64Data);
                if (!bytes)
                {
                    err << fmt::format("{}: {}: image content block was not valid base64\n", label,
                                       tool);
                    return static_cast<int>(ExitCode::ToolError);
                }
                std::ofstream file(*outputPath, std::ios::binary | std::ios::trunc);
                if (!file)
                {
                    err << fmt::format("{}: {}: cannot open {} for writing\n", label, tool,
                                       *outputPath);
                    return static_cast<int>(ExitCode::ToolError);
                }
                file.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
                out << fmt::format("wrote {} bytes ({}) to {}\n", bytes->size(),
                                   call->Image->MimeType, *outputPath);
                // A tool may return both text and an image; emit the text too when present.
                if (!call->Content.empty())
                {
                    out << call->Content << "\n";
                }
                return static_cast<int>(ExitCode::Ok);
            }

            out << call->Content << "\n";
            return static_cast<int>(ExitCode::Ok);
        }
    }

    int RunClientCli(std::span<const string> args, std::ostream& out, std::ostream& err,
                     string_view label)
    {
        optional<Target> target;
        optional<string> toolName;
        bool list = false;
        bool raw = false;
        optional<string> search;
        optional<string> outputPath;
        optional<string> jsonArgs;
        vector<std::pair<string, string>> keyValues;

        for (usize i = 0; i < args.size(); ++i)
        {
            const string& arg = args[i];

            if (arg.rfind("--connect=", 0) == 0)
            {
                const optional<Target> parsed = ParseConnect(string_view(arg).substr(10));
                if (!parsed)
                {
                    return UsageError(
                        err, label, "--connect takes <port> or <host>:<port> with a port 0-65535");
                }
                target = parsed;
            }
            else if (arg == "--list")
            {
                list = true;
            }
            else if (arg == "--raw")
            {
                raw = true;
            }
            else if (arg == "--search")
            {
                if (i + 1 >= args.size())
                {
                    return UsageError(err, label, "--search needs a query argument");
                }
                search = args[++i];
            }
            else if (arg == "--output")
            {
                if (i + 1 >= args.size())
                {
                    return UsageError(err, label, "--output needs a file argument");
                }
                outputPath = args[++i];
            }
            else if (arg == "--json")
            {
                if (i + 1 >= args.size())
                {
                    return UsageError(err, label, "--json needs an object argument");
                }
                jsonArgs = args[++i];
            }
            else if (arg.rfind("--", 0) == 0)
            {
                return UsageError(err, label, fmt::format("unknown option {}", arg));
            }
            else if (const auto eq = arg.find('='); eq != string::npos)
            {
                keyValues.emplace_back(arg.substr(0, eq), arg.substr(eq + 1));
            }
            else if (!toolName)
            {
                toolName = arg;
            }
            else
            {
                return UsageError(err, label, fmt::format("unexpected argument {}", arg));
            }
        }

        if (!target)
        {
            return UsageError(err, label, "--connect=<port|host:port> is required");
        }

        if (list == toolName.has_value())
        {
            return UsageError(err, label, "give exactly one of a <tool-name> or --list");
        }

        if (list)
        {
            if (!keyValues.empty() || jsonArgs || outputPath)
            {
                return UsageError(err, label,
                                  "--list takes only --search; key=value, --json, and --output "
                                  "are tool-call options");
            }
        }
        else
        {
            if (search)
            {
                return UsageError(err, label, "--search is only valid with --list");
            }
            if (jsonArgs && !keyValues.empty())
            {
                return UsageError(err, label,
                                  "--json and key=value arguments are mutually exclusive");
            }
        }

        // Assemble the tool call's `arguments` object into a JSON string for McpClient.
        string argumentsJson = "{}";
        if (!list)
        {
            if (jsonArgs)
            {
                const Json parsed = Json::parse(*jsonArgs, nullptr, false);
                if (!parsed.is_object())
                {
                    return UsageError(err, label, "--json must be a JSON object");
                }
                argumentsJson = DumpSafe(parsed);
            }
            else if (!keyValues.empty())
            {
                Json arguments = Json::object();
                for (const auto& [key, value] : keyValues)
                {
                    // Each value is parsed as JSON when it parses (number/bool/array/object),
                    // else stored as a string — the ergonomic scalar-typing rule.
                    const Json parsed = Json::parse(value, nullptr, false);
                    arguments[key] = parsed.is_discarded() ? Json(value) : parsed;
                }
                argumentsJson = DumpSafe(arguments);
            }
        }

        Mcp::McpClientInfo info;
        info.Host = target->Host;
        info.Port = target->Port;

        Result<Unique<McpClient>> client = McpClient::Create(info);
        if (!client)
        {
            err << fmt::format("{}: cannot reach {}:{}: {}\n", label, target->Host, target->Port,
                               client.error());
            return static_cast<int>(ExitCode::Connect);
        }

        if (list)
        {
            return RunList(**client, out, err, label, *target, search, raw);
        }
        return RunCall(**client, out, err, label, *target, *toolName, argumentsJson, raw,
                       outputPath);
    }
}
