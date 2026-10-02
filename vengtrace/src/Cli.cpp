#include "Cli.h"
#include <Veng/Path.h>

#include <charconv>
#include <fstream>
#include <ostream>
#include <span>
#include <variant>

#include <fmt/format.h>
#include <fmt/ostream.h>

#include "ChromeTraceConverter.h"
#include "Summary.h"
#include "SummaryReport.h"
#include "TraceDecoder.h"

namespace Veng::VengTrace
{
    namespace
    {
        void PrintUsage(std::ostream& sink)
        {
            fmt::print(
                sink,
                "usage:\n"
                "  vengtrace convert <capture> --out <file.json> [--pretty] "
                "[--events complete|pair]\n"
                "  vengtrace summary <capture> [--top N] [--hitches N] [--idle <scope>]... "
                "[--json]\n"
                "  vengtrace compare <a> <b> [--top N] [--json]\n"
                "\n"
                "convert: writes a veng binary capture as Chrome Trace Event JSON, readable\n"
                "  in ui.perfetto.dev and speedscope.app. The JSON is a lossy viewer-facing\n"
                "  projection; the binary capture is the native form.\n"
                "summary: aggregates a capture per frame over its whole frames: the frame\n"
                "  period and work, the steady-state scopes by median exclusive cost, the\n"
                "  longest frames broken down, counters, the GPU's untimed time and where it\n"
                "  falls, and each world's simulation steps. --idle names a scope to leave\n"
                "  out of a frame's work, for a capture older than the idle mark.\n"
                "compare: the per-call median of every scope both captures recorded, with\n"
                "  the ratio a/b. A uniform ratio across unrelated scopes is the signature of\n"
                "  a slowed machine rather than of a code change.\n"
                "\n"
                "exit codes: 0 ok (a truncated capture still converts) | 1 usage |\n"
                "  2 unreadable input | 3 unknown format version | 4 write failure\n");
        }

        int Fail(std::ostream& err, ExitCode code, const string& message)
        {
            fmt::print(err, "vengtrace: {}\n", message);
            return static_cast<int>(code);
        }

        // Reads a whole file into a byte buffer. Returns nullopt when the file cannot be opened —
        // an unreadable input, distinct from a readable file that is not a valid capture.
        optional<vector<u8>> ReadAllBytes(const path& file)
        {
            std::ifstream in(file, std::ios::binary);
            if (!in)
            {
                return std::nullopt;
            }
            return vector<u8>((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        }

        // Reads and decodes a capture, or reports why it could not on err and returns the exit
        // code. A truncated capture decodes, with a warning naming what the caller does with it.
        std::variant<DecodedTrace, int> LoadCapture(const path& capture, string_view onTruncated,
                                                    std::ostream& err)
        {
            const optional<vector<u8>> bytes = ReadAllBytes(capture);
            if (!bytes)
            {
                return Fail(err, ExitCode::Unreadable,
                            fmt::format("cannot read '{}'", capture.string()));
            }
            DecodeResult decoded = Decode(std::span<const u8>(*bytes));
            switch (decoded.Status)
            {
            case DecodeStatus::NotACapture:
                return Fail(err, ExitCode::Unreadable,
                            fmt::format("'{}' is not a veng capture", capture.string()));
            case DecodeStatus::UnknownVersion:
                return Fail(
                    err, ExitCode::UnknownVersion,
                    fmt::format("'{}' is format version {}, which this tool does not support",
                                capture.string(), decoded.FormatVersion));
            case DecodeStatus::Ok:
                break;
            }
            if (decoded.Trace.Truncated)
            {
                fmt::print(err, "vengtrace: '{}' is truncated (no trailer); {}\n", capture.string(),
                           onTruncated);
            }
            return std::move(decoded.Trace);
        }

        // Parses a positive count option's value, reporting a bad one as a usage error.
        optional<u32> ParseCount(const string& option, const string& value, std::ostream& err)
        {
            u32 count = 0;
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), count);
            if (error != std::errc() || end != value.data() + value.size())
            {
                fmt::print(err, "vengtrace: {} expects a count, got '{}'\n", option, value);
                return std::nullopt;
            }
            return count;
        }

        // The options summary and compare share — --top, --json and positional captures — and the
        // two only summary takes, --hitches and --idle.
        struct ReportArgs
        {
            vector<path> Captures;
            SummaryOptions Options;
            bool TopGiven = false;
            bool Json = false;
        };

        // Parses a report subcommand's arguments; returns an exit code when parsing ends the run.
        optional<int> ParseReportArgs(const vector<string>& args, bool isSummary,
                                      ReportArgs& parsed, std::ostream& out, std::ostream& err)
        {
            for (usize i = 1; i < args.size(); ++i)
            {
                const string& arg = args[i];
                const bool isTop = arg == "--top";
                if (isTop || (isSummary && arg == "--hitches"))
                {
                    if (i + 1 >= args.size())
                    {
                        PrintUsage(err);
                        return static_cast<int>(ExitCode::Usage);
                    }
                    const optional<u32> count = ParseCount(arg, args[++i], err);
                    if (!count)
                    {
                        return static_cast<int>(ExitCode::Usage);
                    }
                    (isTop ? parsed.Options.Top : parsed.Options.Hitches) = *count;
                    parsed.TopGiven = parsed.TopGiven || isTop;
                }
                else if (arg == "--json")
                {
                    parsed.Json = true;
                }
                else if (isSummary && arg == "--idle")
                {
                    if (i + 1 >= args.size())
                    {
                        PrintUsage(err);
                        return static_cast<int>(ExitCode::Usage);
                    }
                    parsed.Options.IdleScopes.push_back(args[++i]);
                }
                else if (arg == "--help" || arg == "-h")
                {
                    PrintUsage(out);
                    return static_cast<int>(ExitCode::Ok);
                }
                else if (arg.rfind("--", 0) == 0)
                {
                    fmt::print(err, "vengtrace: unknown option '{}'\n", arg);
                    return static_cast<int>(ExitCode::Usage);
                }
                else
                {
                    parsed.Captures.emplace_back(arg);
                }
            }
            return std::nullopt;
        }

        int RunSummary(const vector<string>& args, std::ostream& out, std::ostream& err)
        {
            ReportArgs parsed;
            if (const optional<int> exit = ParseReportArgs(args, true, parsed, out, err))
            {
                return *exit;
            }
            if (parsed.Captures.size() != 1)
            {
                PrintUsage(err);
                return static_cast<int>(ExitCode::Usage);
            }
            std::variant<DecodedTrace, int> loaded =
                LoadCapture(parsed.Captures[0], "summarizing the recovered sections", err);
            if (const int* exit = std::get_if<int>(&loaded))
            {
                return *exit;
            }
            const CaptureSummary summary =
                Summarize(std::get<DecodedTrace>(loaded), parsed.Options);
            const string label = parsed.Captures[0].string();
            fmt::print(out, "{}",
                       parsed.Json ? SummaryToJson(summary, label) : FormatSummary(summary, label));
            return static_cast<int>(ExitCode::Ok);
        }

        int RunCompare(const vector<string>& args, std::ostream& out, std::ostream& err)
        {
            ReportArgs parsed;
            if (const optional<int> exit = ParseReportArgs(args, false, parsed, out, err))
            {
                return *exit;
            }
            if (parsed.Captures.size() != 2)
            {
                PrintUsage(err);
                return static_cast<int>(ExitCode::Usage);
            }
            std::variant<DecodedTrace, int> a =
                LoadCapture(parsed.Captures[0], "comparing the recovered sections", err);
            if (const int* exit = std::get_if<int>(&a))
            {
                return *exit;
            }
            std::variant<DecodedTrace, int> b =
                LoadCapture(parsed.Captures[1], "comparing the recovered sections", err);
            if (const int* exit = std::get_if<int>(&b))
            {
                return *exit;
            }
            const Comparison comparison =
                Compare(std::get<DecodedTrace>(a), std::get<DecodedTrace>(b));
            const string labelA = parsed.Captures[0].string();
            const string labelB = parsed.Captures[1].string();
            // Every common scope by default: a comparison is read for its uniformity, which a
            // trimmed table would hide.
            fmt::print(out, "{}",
                       parsed.Json ? ComparisonToJson(comparison, labelA, labelB)
                                   : FormatComparison(comparison, labelA, labelB,
                                                      parsed.TopGiven ? parsed.Options.Top : 0));
            return static_cast<int>(ExitCode::Ok);
        }

        int RunConvert(const vector<string>& args, std::ostream& out, std::ostream& err)
        {
            optional<path> capturePath;
            optional<path> outPath;
            ConvertOptions options;

            for (usize i = 1; i < args.size(); ++i)
            {
                const string& arg = args[i];
                if (arg == "--out" || arg == "-o")
                {
                    if (i + 1 >= args.size())
                    {
                        PrintUsage(err);
                        return static_cast<int>(ExitCode::Usage);
                    }
                    outPath = path(args[++i]);
                }
                else if (arg == "--pretty")
                {
                    options.Pretty = true;
                }
                else if (arg == "--events")
                {
                    if (i + 1 >= args.size())
                    {
                        PrintUsage(err);
                        return static_cast<int>(ExitCode::Usage);
                    }
                    const string& value = args[++i];
                    if (value == "complete")
                    {
                        options.Events = EventForm::Complete;
                    }
                    else if (value == "pair")
                    {
                        options.Events = EventForm::Pair;
                    }
                    else
                    {
                        fmt::print(err,
                                   "vengtrace: --events expects 'complete' or 'pair', got "
                                   "'{}'\n",
                                   value);
                        return static_cast<int>(ExitCode::Usage);
                    }
                }
                else if (arg == "--help" || arg == "-h")
                {
                    PrintUsage(out);
                    return static_cast<int>(ExitCode::Ok);
                }
                else if (arg.rfind("--", 0) == 0)
                {
                    fmt::print(err, "vengtrace: unknown option '{}'\n", arg);
                    return static_cast<int>(ExitCode::Usage);
                }
                else if (!capturePath)
                {
                    capturePath = path(arg);
                }
                else
                {
                    fmt::print(err, "vengtrace: unexpected argument '{}'\n", arg);
                    return static_cast<int>(ExitCode::Usage);
                }
            }

            if (!capturePath || !outPath)
            {
                PrintUsage(err);
                return static_cast<int>(ExitCode::Usage);
            }

            const optional<vector<u8>> bytes = ReadAllBytes(*capturePath);
            if (!bytes)
            {
                return Fail(err, ExitCode::Unreadable,
                            fmt::format("cannot read '{}'", capturePath->string()));
            }

            const DecodeResult decoded = Decode(std::span<const u8>(*bytes));
            switch (decoded.Status)
            {
            case DecodeStatus::NotACapture:
                return Fail(err, ExitCode::Unreadable,
                            fmt::format("'{}' is not a veng capture", capturePath->string()));
            case DecodeStatus::UnknownVersion:
                return Fail(
                    err, ExitCode::UnknownVersion,
                    fmt::format("'{}' is format version {}, which this tool does not support",
                                capturePath->string(), decoded.FormatVersion));
            case DecodeStatus::Ok:
                break;
            }

            // A truncated capture is not an error: it converts to valid partial JSON with the
            // truncation recorded. Warn on stderr so the loss is visible, then carry on.
            if (decoded.Trace.Truncated)
            {
                fmt::print(err,
                           "vengtrace: '{}' is truncated (no trailer); converting the recovered "
                           "sections\n",
                           capturePath->string());
            }

            const string json = ConvertToChromeTrace(decoded.Trace, options);

            std::ofstream output(*outPath, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                return Fail(err, ExitCode::WriteFailure,
                            fmt::format("cannot write '{}'", outPath->string()));
            }
            output.write(json.data(), static_cast<std::streamsize>(json.size()));
            if (!output)
            {
                return Fail(err, ExitCode::WriteFailure,
                            fmt::format("failed writing '{}'", outPath->string()));
            }

            fmt::print(out, "vengtrace: converted '{}' -> '{}' ({} events)\n",
                       capturePath->string(), outPath->string(), decoded.Trace.Events.size());
            return static_cast<int>(ExitCode::Ok);
        }
    }

    int RunVengtraceCli(const vector<string>& args, std::ostream& out, std::ostream& err)
    {
        if (args.empty())
        {
            PrintUsage(err);
            return static_cast<int>(ExitCode::Usage);
        }

        const string& subcommand = args[0];
        if (subcommand == "convert")
        {
            return RunConvert(args, out, err);
        }
        if (subcommand == "summary")
        {
            return RunSummary(args, out, err);
        }
        if (subcommand == "compare")
        {
            return RunCompare(args, out, err);
        }
        if (subcommand == "--help" || subcommand == "-h" || subcommand == "help")
        {
            PrintUsage(out);
            return static_cast<int>(ExitCode::Ok);
        }

        fmt::print(err, "vengtrace: unknown subcommand '{}'\n", subcommand);
        PrintUsage(err);
        return static_cast<int>(ExitCode::Usage);
    }
}
