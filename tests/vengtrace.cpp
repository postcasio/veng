// vengtrace conformance: the binary-capture -> Chrome Trace JSON converter, exercised over plan
// 01's committed reference fixture. The decoder is written against docs/trace-format.md (not the
// engine's writer), so these cases pin the projection: every record type maps with the right shape,
// event counts and durations round-trip with no drop or duplication, a back-dated GPU pass lands
// under the frame it measured, the frame ruler and the virtual lanes emit async slices whose
// cookies keep same-cookie slices properly nested, drop/truncation accounting travels into the
// JSON, a truncated capture converts to valid partial JSON with its trailing frame left open, an
// unknown section is skipped, an unknown version is rejected, and the CLI arg grammar + exit-code
// map are asserted in-process (mirroring how mcp_cli drives RunClientCli).
//
// Device-free pure logic + a committed fixture, so it runs in the default (fast) band.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstring>
#include <fstream>
#include <span>
#include <sstream>

#include <nlohmann/json.hpp>

#include "ChromeTraceConverter.h"
#include "Cli.h"
#include "TraceDecoder.h"
#include "support/TempPath.h"

using namespace Veng;
using namespace Veng::VengTrace;
using Json = nlohmann::json;

namespace
{
    vector<u8> ReadBytes(const std::filesystem::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        return vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    std::filesystem::path FixtureDir()
    {
        return std::filesystem::path(VENG_TEST_FIXTURE_DIR);
    }
    std::filesystem::path CompleteFixture()
    {
        return FixtureDir() / "trace-fixture.vtrace";
    }
    std::filesystem::path TruncatedFixture()
    {
        return FixtureDir() / "trace-fixture-truncated.vtrace";
    }

    DecodedTrace DecodeFixture(const std::filesystem::path& file)
    {
        const vector<u8> bytes = ReadBytes(file);
        const DecodeResult result = Decode(std::span<const u8>(bytes));
        REQUIRE(result.Status == DecodeStatus::Ok);
        return result.Trace;
    }

    // Every traceEvents entry matching a phase, optionally on a given tid.
    vector<Json> EventsWithPhase(const Json& document, string_view phase)
    {
        vector<Json> matches;
        for (const Json& event : document.at("traceEvents"))
        {
            if (event.value("ph", string()) == phase)
            {
                matches.push_back(event);
            }
        }
        return matches;
    }

    // One async slice, reassembled from its b/e pair. An open slice (a truncated capture's trailing
    // frame) has no End.
    struct AsyncSpan
    {
        i64 Id = 0;
        i64 Tid = 0;
        string Name;
        string Category;
        f64 Begin = 0.0;
        optional<f64> End;
    };

    // Pairs every b with the e that closes it, matching on cookie the way a viewer does.
    vector<AsyncSpan> AsyncSpans(const Json& document)
    {
        vector<AsyncSpan> spans;
        std::map<i64, vector<usize>> open;
        for (const Json& event : document.at("traceEvents"))
        {
            const string phase = event.value("ph", string());
            if (phase == "b")
            {
                spans.push_back(AsyncSpan{.Id = event.at("id").get<i64>(),
                                          .Tid = event.value("tid", i64{0}),
                                          .Name = event.value("name", string()),
                                          .Category = event.value("cat", string()),
                                          .Begin = event.at("ts").get<f64>()});
                open[spans.back().Id].push_back(spans.size() - 1);
            }
            else if (phase == "e")
            {
                vector<usize>& stack = open[event.at("id").get<i64>()];
                REQUIRE_FALSE(stack.empty()); // an end with no begin would be unmatchable
                AsyncSpan& span = spans[stack.back()];
                stack.pop_back();
                CHECK(span.Name == event.value("name", string()));
                CHECK(span.Category == event.value("cat", string()));
                span.End = event.at("ts").get<f64>();
            }
        }
        return spans;
    }

    optional<Json> FindEvent(const Json& document, string_view phase, string_view name)
    {
        for (const Json& event : document.at("traceEvents"))
        {
            if (event.value("ph", string()) == phase && event.value("name", string()) == name)
            {
                return event;
            }
        }
        return std::nullopt;
    }
}

TEST_CASE(
    "vengtrace: the reference fixture converts to parseable JSON with every record type mapped")
{
    const DecodedTrace trace = DecodeFixture(CompleteFixture());
    CHECK(trace.Complete);
    CHECK_FALSE(trace.Truncated);

    const string json = ConvertToChromeTrace(trace, {});
    const Json document = Json::parse(json, nullptr, false);
    REQUIRE_FALSE(document.is_discarded());
    REQUIRE(document.contains("traceEvents"));

    // The fixture carries 5 scope records, 3 counters, 1 instant. Four scopes sit on thread lanes
    // and become one X each; the fifth is on the virtual GPU lane and becomes an async b/e pair, as
    // does each of the 4 frames (9, 10, 11, 12) on the frame track.
    const vector<Json> completes = EventsWithPhase(document, "X");
    for (const Json& event : completes)
    {
        // No complete event survives on the frame track or a virtual lane.
        CHECK(event.value("tid", i64{0}) < i64{1'000'000'000});
    }
    CHECK(completes.size() == 4); // event counts round-trip: no drop, no duplication
    CHECK(EventsWithPhase(document, "b").size() == 5); // 4 frames + the GPU pass
    CHECK(EventsWithPhase(document, "e").size() == 5);
    CHECK(EventsWithPhase(document, "C").size() == 3); // counters
    CHECK(EventsWithPhase(document, "i").size() == 1); // instant

    for (const Json& event : EventsWithPhase(document, "b"))
    {
        // An async slice states its lane by cookie and repeats name + category on the end, which is
        // what lets a viewer match the pair without guessing.
        CHECK(event.contains("id"));
        CHECK(event.contains("cat"));
    }

    // A scope's duration round-trips: "Update" spans 150 ticks at 24 MHz = 6.25 us.
    const optional<Json> update = FindEvent(document, "X", "Update");
    REQUIRE(update.has_value());
    CHECK(update->at("dur").get<f64>() == doctest::Approx(150.0 * 1'000'000.0 / 24'000'000.0));
    CHECK(update->at("args").at("frame").get<u64>() == 10);
}

TEST_CASE("vengtrace: counters map to C events carrying their narrowest value form")
{
    const DecodedTrace trace = DecodeFixture(CompleteFixture());
    const Json document = Json::parse(ConvertToChromeTrace(trace, {}), nullptr, false);

    const optional<Json> draws = FindEvent(document, "C", "draw.calls");
    REQUIRE(draws.has_value());
    CHECK(draws->at("args").at("draw.calls").is_number_integer());
    CHECK(draws->at("args").at("draw.calls").get<i64>() == 1234);

    const optional<Json> queue = FindEvent(document, "C", "queue.depth");
    REQUIRE(queue.has_value());
    CHECK(queue->at("args").at("queue.depth").get<i64>() == -7);

    const optional<Json> gpuMs = FindEvent(document, "C", "gpu.ms");
    REQUIRE(gpuMs.has_value());
    CHECK(gpuMs->at("args").at("gpu.ms").get<f64>() == doctest::Approx(3.5));
}

TEST_CASE("vengtrace: tracks become thread_name metadata ordered by role")
{
    const DecodedTrace trace = DecodeFixture(CompleteFixture());
    const Json document = Json::parse(ConvertToChromeTrace(trace, {}), nullptr, false);

    // A tid -> {name, sort_index} view built from the metadata events.
    std::map<i64, string> names;
    std::map<i64, i64> sorts;
    for (const Json& event : EventsWithPhase(document, "M"))
    {
        if (event.value("name", string()) == "thread_name")
        {
            names[event.value("tid", i64{0})] = event.at("args").value("name", string());
        }
        else if (event.value("name", string()) == "thread_sort_index")
        {
            sorts[event.value("tid", i64{0})] = event.at("args").value("sort_index", i64{0});
        }
    }

    CHECK(names[1] == "Main");
    CHECK(names[2] == "Worker 0");
    CHECK(names[1'000'000'001] == "GPU");    // the virtual GPU track is its own pseudo-thread
    CHECK(names[2'000'000'000] == "Frames"); // the dedicated frame track

    // The frame ruler reads at the top; the GPU lane sits below the CPU threads.
    CHECK(sorts[2'000'000'000] < sorts[1]);
    CHECK(sorts[1] < sorts[2]);
    CHECK(sorts[2] < sorts[1'000'000'001]);
}

TEST_CASE("vengtrace: a back-dated GPU pass lands under the frame it measured")
{
    const DecodedTrace trace = DecodeFixture(CompleteFixture());
    const Json document = Json::parse(ConvertToChromeTrace(trace, {}), nullptr, false);

    // The GPU pass is an async slice on the virtual GPU pseudo-thread; it measures frame 9, a frame
    // earlier than the chunk it was recorded in.
    const vector<AsyncSpan> spans = AsyncSpans(document);
    const auto find = [&spans](string_view name) -> optional<AsyncSpan>
    {
        for (const AsyncSpan& span : spans)
        {
            if (span.Name == name)
            {
                return span;
            }
        }
        return std::nullopt;
    };

    const optional<AsyncSpan> gpu = find("GpuPass");
    REQUIRE(gpu.has_value());
    CHECK(gpu->Tid == i64{1'000'000'001});
    CHECK(gpu->Category == "gpu");
    CHECK(FindEvent(document, "b", "GpuPass")->at("args").at("frame").get<u64>() == 9);

    const optional<AsyncSpan> frame9 = find("Frame 9");
    REQUIRE(frame9.has_value());
    CHECK(frame9->Tid == i64{2'000'000'000});

    REQUIRE(gpu->End.has_value());
    REQUIRE(frame9->End.has_value());
    CHECK(gpu->Begin >= frame9->Begin);
    CHECK(*gpu->End <= *frame9->End);
}

TEST_CASE("vengtrace: async slices sharing a cookie are properly nested")
{
    const DecodedTrace trace = DecodeFixture(CompleteFixture());
    const Json document = Json::parse(ConvertToChromeTrace(trace, {}), nullptr, false);

    // The whole point of the cookie: a viewer never has to infer parenthood from containment, so
    // slices that share a cookie must nest exactly, and any pair that merely overlaps must sit on
    // different cookies. Distinct cookies may overlap freely — that is what makes pipelined frames
    // and interleaved GPU passes representable.
    std::map<i64, vector<AsyncSpan>> byCookie;
    for (const AsyncSpan& span : AsyncSpans(document))
    {
        byCookie[span.Id].push_back(span);
    }
    for (auto& [cookie, spans] : byCookie)
    {
        for (usize outer = 0; outer < spans.size(); ++outer)
        {
            for (usize inner = outer + 1; inner < spans.size(); ++inner)
            {
                const AsyncSpan& a = spans[outer];
                const AsyncSpan& b = spans[inner];
                REQUIRE(a.End.has_value());
                REQUIRE(b.End.has_value());
                const bool disjoint = (*a.End <= b.Begin) || (*b.End <= a.Begin);
                const bool nested = (a.Begin <= b.Begin && *b.End <= *a.End) ||
                                    (b.Begin <= a.Begin && *a.End <= *b.End);
                CHECK((disjoint || nested));
            }
        }
    }
}

TEST_CASE("vengtrace: drop and provenance accounting travel into the JSON")
{
    const DecodedTrace trace = DecodeFixture(CompleteFixture());
    const Json document = Json::parse(ConvertToChromeTrace(trace, {}), nullptr, false);
    const Json& other = document.at("otherData");

    CHECK(other.at("complete").get<bool>());
    CHECK_FALSE(other.at("truncated").get<bool>());
    CHECK(other.at("droppedEvents").get<u64>() == 42);
    CHECK(other.at("droppedThreads").get<u64>() == 1);
    CHECK(other.at("formatVersion").get<u32>() == 1);
    CHECK(other.at("tickFrequency").get<u64>() == 24'000'000);
    CHECK(other.at("captureMode").get<string>() == "ring");
    CHECK(other.at("engineVersion").get<string>() == "0.0.0-fixture");
    CHECK(other.at("executable").get<string>() == "trace-fixture");
    REQUIRE(other.at("submodules").size() == 2);
    CHECK(other.at("submodules")[0].at("name").get<string>() == "engine");
    CHECK(other.at("submodules")[1].at("dirty").get<bool>());
}

TEST_CASE(
    "vengtrace: a truncated capture converts with the truncation recorded and its last frame open")
{
    const vector<u8> bytes = ReadBytes(TruncatedFixture());
    const DecodeResult result = Decode(std::span<const u8>(bytes));
    REQUIRE(result.Status == DecodeStatus::Ok);
    CHECK(result.Trace.Truncated);
    CHECK_FALSE(result.Trace.Complete);

    const Json document = Json::parse(ConvertToChromeTrace(result.Trace, {}), nullptr, false);
    REQUIRE_FALSE(document.is_discarded()); // valid, well-formed partial JSON

    CHECK(document.at("otherData").at("truncated").get<bool>());

    // The loss is visible without reading otherData: a process_labels metadata event says so.
    bool sawTruncatedLabel = false;
    for (const Json& event : EventsWithPhase(document, "M"))
    {
        if (event.value("name", string()) == "process_labels" &&
            event.at("args").value("labels", string()).find("truncated") != string::npos)
        {
            sawTruncatedLabel = true;
        }
    }
    CHECK(sawTruncatedLabel);

    // Only the first chunk survived the cut: 3 scopes, 3 counters, 1 instant, frames 10 and 11.
    usize scopeCount = 0;
    for (const Json& event : EventsWithPhase(document, "X"))
    {
        if (event.value("tid", i64{0}) != i64{2'000'000'000})
        {
            ++scopeCount;
        }
    }
    CHECK(scopeCount == 3);

    // The trailing frame (11) was still open at the cut, so its async slice is a begin with no
    // matching end and runs to the end of the trace; the earlier frame (10) closes normally.
    const vector<AsyncSpan> spans = AsyncSpans(document);
    usize openFrames = 0;
    for (const AsyncSpan& span : spans)
    {
        if (span.Name == "Frame 10")
        {
            CHECK(span.End.has_value());
        }
        if (span.Name == "Frame 11")
        {
            CHECK_FALSE(span.End.has_value());
            ++openFrames;
        }
    }
    CHECK(openFrames == 1);
    CHECK_FALSE(FindEvent(document, "e", "Frame 11").has_value());
}

TEST_CASE("vengtrace: an unknown section is skipped and the capture still converts")
{
    // Splice an unrecognized section (type 9999) in front of the fixture's trailer. The last section
    // before the trailer is the Accounting section; inserting just before the trailer keeps the
    // stream well-framed, so the decoder skips the unknown section and reaches the trailer.
    vector<u8> bytes = ReadBytes(CompleteFixture());

    // Find the trailer: the fixture's final section is the 8-byte Trailer header with no payload.
    REQUIRE(bytes.size() >= 8);
    const usize trailerStart = bytes.size() - 8;

    vector<u8> unknown;
    const auto putU32 = [&unknown](u32 value)
    {
        for (u32 i = 0; i < 4; ++i)
        {
            unknown.push_back(static_cast<u8>(value >> (i * 8)));
        }
    };
    putU32(9999); // an unknown section type
    putU32(3);    // payload length
    unknown.insert(unknown.end(), {0xAA, 0xBB, 0xCC});

    vector<u8> spliced(bytes.begin(), bytes.begin() + trailerStart);
    spliced.insert(spliced.end(), unknown.begin(), unknown.end());
    spliced.insert(spliced.end(), bytes.begin() + trailerStart, bytes.end());

    const DecodeResult result = Decode(std::span<const u8>(spliced));
    REQUIRE(result.Status == DecodeStatus::Ok);
    CHECK(result.Trace.Complete);
    CHECK_FALSE(result.Trace.Truncated);
    REQUIRE(result.Trace.UnknownSections.size() == 1);
    CHECK(result.Trace.UnknownSections[0] == 9999);

    const Json document = Json::parse(ConvertToChromeTrace(result.Trace, {}), nullptr, false);
    REQUIRE_FALSE(document.is_discarded());
    CHECK(document.at("otherData").at("unknownSections")[0].get<u32>() == 9999);
}

TEST_CASE("vengtrace: an unknown format version is rejected by the decoder")
{
    vector<u8> bytes = ReadBytes(CompleteFixture());
    REQUIRE(bytes.size() > 8);
    bytes[8] = 0xFF; // the format version field is at offset 8, just past the magic

    const DecodeResult result = Decode(std::span<const u8>(bytes));
    CHECK(result.Status == DecodeStatus::UnknownVersion);
}

TEST_CASE("vengtrace: --events pair emits begin/end pairs")
{
    const DecodedTrace trace = DecodeFixture(CompleteFixture());
    const Json document =
        Json::parse(ConvertToChromeTrace(trace, {.Events = EventForm::Pair}), nullptr, false);

    // Each of the 4 thread-lane scopes becomes a B and an E, and no scope survives as an X. The
    // fifth scope is on the virtual GPU lane, which is async in either form.
    CHECK(EventsWithPhase(document, "B").size() == 4);
    CHECK(EventsWithPhase(document, "E").size() == 4);
    CHECK(EventsWithPhase(document, "X").empty());
    CHECK(EventsWithPhase(document, "b").size() == 5);
}

// ---- The CLI arg grammar and exit-code contract, driven in-process ----------------------------

namespace
{
    struct CliRun
    {
        int Code = 0;
        string Out;
        string Err;
    };

    CliRun RunCli(const vector<string>& args)
    {
        std::ostringstream out;
        std::ostringstream err;
        const int code = RunVengtraceCli(args, out, err);
        return CliRun{.Code = code, .Out = out.str(), .Err = err.str()};
    }

    std::filesystem::path OutPath(string_view stem)
    {
        return Veng::TestSupport::TempDir() / (string(stem) + ".json");
    }
}

TEST_CASE("vengtrace CLI: a clean conversion exits 0 and writes parseable JSON")
{
    const std::filesystem::path out = OutPath("cli-basic");
    const CliRun run = RunCli({"convert", CompleteFixture().string(), "--out", out.string()});
    CHECK(run.Code == static_cast<int>(ExitCode::Ok));
    CHECK(run.Err.empty());
    CHECK(run.Out.find("converted") != string::npos);

    const Json document = Json::parse(ReadBytes(out), nullptr, false);
    REQUIRE_FALSE(document.is_discarded());
    CHECK(document.contains("traceEvents"));
}

TEST_CASE("vengtrace CLI: --pretty indents and --events pair selects the pair form")
{
    const std::filesystem::path pretty = OutPath("cli-pretty");
    const CliRun prettyRun =
        RunCli({"convert", CompleteFixture().string(), "--out", pretty.string(), "--pretty"});
    CHECK(prettyRun.Code == static_cast<int>(ExitCode::Ok));
    const vector<u8> prettyBytes = ReadBytes(pretty);
    const string prettyText(prettyBytes.begin(), prettyBytes.end());
    CHECK(prettyText.find('\n') != string::npos); // compact output carries no newline

    const std::filesystem::path pair = OutPath("cli-pair");
    const CliRun pairRun =
        RunCli({"convert", CompleteFixture().string(), "--out", pair.string(), "--events", "pair"});
    CHECK(pairRun.Code == static_cast<int>(ExitCode::Ok));
    const Json document = Json::parse(ReadBytes(pair), nullptr, false);
    CHECK(EventsWithPhase(document, "E").size() == 4);
}

TEST_CASE("vengtrace CLI: a truncated capture is not an error")
{
    const std::filesystem::path out = OutPath("cli-truncated");
    const CliRun run = RunCli({"convert", TruncatedFixture().string(), "--out", out.string()});
    CHECK(run.Code == static_cast<int>(ExitCode::Ok)); // truncation converts, it does not fail
    CHECK(run.Err.find("truncated") != string::npos);  // but it warns on stderr
    const Json document = Json::parse(ReadBytes(out), nullptr, false);
    REQUIRE_FALSE(document.is_discarded());
}

TEST_CASE("vengtrace CLI: usage errors exit 1")
{
    CHECK(RunCli({}).Code == static_cast<int>(ExitCode::Usage));
    CHECK(RunCli({"bogus"}).Code == static_cast<int>(ExitCode::Usage));
    // Missing the capture positional.
    CHECK(RunCli({"convert", "--out", "x.json"}).Code == static_cast<int>(ExitCode::Usage));
    // Missing --out.
    CHECK(RunCli({"convert", CompleteFixture().string()}).Code ==
          static_cast<int>(ExitCode::Usage));
    // A bad --events value.
    CHECK(RunCli({"convert", CompleteFixture().string(), "--out", "x.json", "--events", "wat"})
              .Code == static_cast<int>(ExitCode::Usage));
    // An unknown option.
    CHECK(RunCli({"convert", CompleteFixture().string(), "--out", "x.json", "--frob"}).Code ==
          static_cast<int>(ExitCode::Usage));
}

TEST_CASE("vengtrace CLI: an unreadable input exits 2")
{
    const CliRun run =
        RunCli({"convert", "/no/such/capture.vtrace", "--out", OutPath("cli-missing").string()});
    CHECK(run.Code == static_cast<int>(ExitCode::Unreadable));
}

TEST_CASE("vengtrace CLI: an unknown format version exits 3")
{
    vector<u8> bytes = ReadBytes(CompleteFixture());
    REQUIRE(bytes.size() > 8);
    bytes[8] = 0xFF;
    const std::filesystem::path bad = Veng::TestSupport::TempDir() / "cli-badversion.vtrace";
    {
        std::ofstream file(bad, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    const CliRun run =
        RunCli({"convert", bad.string(), "--out", OutPath("cli-badversion").string()});
    CHECK(run.Code == static_cast<int>(ExitCode::UnknownVersion));
}

TEST_CASE("vengtrace CLI: an unwritable output exits 4")
{
    const CliRun run =
        RunCli({"convert", CompleteFixture().string(), "--out", "/no/such/directory/out.json"});
    CHECK(run.Code == static_cast<int>(ExitCode::WriteFailure));
}
