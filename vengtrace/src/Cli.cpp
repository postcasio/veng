#include "Cli.h"

#include <fstream>
#include <ostream>
#include <span>

#include <fmt/format.h>
#include <fmt/ostream.h>

#include "ChromeTraceConverter.h"
#include "TraceDecoder.h"

namespace Veng::VengTrace
{
    namespace
    {
        void PrintUsage(std::ostream& sink)
        {
            fmt::print(sink,
                       "usage:\n"
                       "  vengtrace convert <capture> --out <file.json> [--pretty] "
                       "[--events complete|pair]\n"
                       "\n"
                       "Converts a veng binary capture to Chrome Trace Event JSON, readable in\n"
                       "ui.perfetto.dev and speedscope.app. The JSON is a lossy viewer-facing\n"
                       "projection; the binary capture is the native form.\n"
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
