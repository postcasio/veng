#include "ChromeTraceConverter.h"

#include <algorithm>
#include <limits>
#include <map>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace Veng::VengTrace
{
    namespace
    {
        using Json = nlohmann::json;

        // Chrome Trace has one process; every track is a thread under it. Thread-track ids are the
        // profiler's small track ids, so the virtual and frame lanes take high, non-overlapping ids.
        constexpr i64 ProcessId = 1;
        constexpr i64 VirtualTidBase = 1'000'000'000;
        constexpr i64 FrameTid = 2'000'000'000;

        i64 ThreadTid(u32 threadId)
        {
            return static_cast<i64>(threadId);
        }
        i64 VirtualTid(u32 virtualId)
        {
            return VirtualTidBase + static_cast<i64>(virtualId);
        }

        // Role groups the lanes top-to-bottom: the frame ruler first, then CPU threads (main lowest,
        // workers after it in id order), then GPU and custom lanes below. Chrome sorts ascending.
        i64 SortIndex(TrackKind kind, TrackRole role, u32 id)
        {
            i64 base = 0;
            switch (role)
            {
            case TrackRole::Cpu:
                base = 100;
                break;
            case TrackRole::Custom:
                base = 1'000;
                break;
            case TrackRole::Gpu:
                base = 10'000;
                break;
            }
            if (kind == TrackKind::Virtual)
            {
                base += 100'000;
            }
            return base + static_cast<i64>(id);
        }

        string EventName(const DecodedTrace& trace, const Event& event)
        {
            const string_view name = trace.Resolve(event.Name);
            if (!name.empty())
            {
                return string(name);
            }
            switch (event.Type)
            {
            case RecordType::Counter:
                return "counter";
            case RecordType::Instant:
                return "instant";
            case RecordType::ScopeComplete:
                break;
            }
            return "scope";
        }

        // A counter's sample is emitted in its narrowest JSON form: an integer tag stays an integer
        // (a queue depth reads as 5, not 5.0), a raw-f64 tag stays a double.
        Json CounterValue(const Event& event)
        {
            switch (event.ValueTag)
            {
            case CounterValueTag::VarU64:
                return Json(static_cast<u64>(event.Value));
            case CounterValueTag::ZigzagI64:
                return Json(static_cast<i64>(event.Value));
            case CounterValueTag::RawF64:
                break;
            }
            return Json(event.Value);
        }
    }

    string ConvertToChromeTrace(const DecodedTrace& trace, const ConvertOptions& options)
    {
        // Chrome Trace timestamps are only meaningful relative to one another, so rebase to the
        // earliest event and express microseconds as floating point (a sub-microsecond scope must
        // not collapse to a zero duration).
        const f64 frequency =
            (trace.TickFrequency != 0) ? static_cast<f64>(trace.TickFrequency) : 1.0;
        u64 minTick = std::numeric_limits<u64>::max();
        for (const Event& event : trace.Events)
        {
            minTick = std::min(minTick, event.BeginTicks);
        }
        if (trace.Events.empty())
        {
            minTick = 0;
        }
        const auto toMicros = [&](u64 tick) -> f64
        { return static_cast<f64>(tick - minTick) * 1'000'000.0 / frequency; };
        const auto durMicros = [&](u64 begin, u64 end) -> f64
        { return static_cast<f64>(end - begin) * 1'000'000.0 / frequency; };

        Json events = Json::array();

        // ---- Process and thread metadata -------------------------------------------------------
        {
            Json processName;
            processName["ph"] = "M";
            processName["name"] = "process_name";
            processName["pid"] = ProcessId;
            const string label = (trace.Meta && !trace.Meta->ExecutableBasename.empty())
                                     ? fmt::format("{} capture", trace.Meta->ExecutableBasename)
                                     : string("veng capture");
            processName["args"] = Json{{"name", label}};
            events.push_back(std::move(processName));
        }

        // Drop/truncation accounting rides a process_labels event too, so a viewer user who never
        // read the process metadata still sees that a capture was lossy or cut short.
        if (trace.Truncated ||
            (trace.HasAccounting && (trace.DroppedEvents || trace.DroppedThreads)))
        {
            vector<string> parts;
            if (trace.Truncated)
            {
                parts.emplace_back("truncated");
            }
            if (trace.HasAccounting && trace.DroppedEvents)
            {
                parts.push_back(fmt::format("droppedEvents={}", trace.DroppedEvents));
            }
            if (trace.HasAccounting && trace.DroppedThreads)
            {
                parts.push_back(fmt::format("droppedThreads={}", trace.DroppedThreads));
            }
            string joined;
            for (const string& part : parts)
            {
                if (!joined.empty())
                {
                    joined += "; ";
                }
                joined += part;
            }
            Json labels;
            labels["ph"] = "M";
            labels["name"] = "process_labels";
            labels["pid"] = ProcessId;
            labels["args"] = Json{{"labels", joined}};
            events.push_back(std::move(labels));
        }

        const auto emitThreadMeta = [&](i64 tid, const string& name, i64 sortIndex)
        {
            Json threadName;
            threadName["ph"] = "M";
            threadName["name"] = "thread_name";
            threadName["pid"] = ProcessId;
            threadName["tid"] = tid;
            threadName["args"] = Json{{"name", name}};
            events.push_back(std::move(threadName));

            Json threadSort;
            threadSort["ph"] = "M";
            threadSort["name"] = "thread_sort_index";
            threadSort["pid"] = ProcessId;
            threadSort["tid"] = tid;
            threadSort["args"] = Json{{"sort_index", sortIndex}};
            events.push_back(std::move(threadSort));
        };

        // The frame ruler sits above every recording lane.
        emitThreadMeta(FrameTid, "Frames", 0);

        for (const Track& track : trace.Tracks)
        {
            const i64 tid =
                (track.Kind == TrackKind::Virtual) ? VirtualTid(track.Id) : ThreadTid(track.Id);
            const string name =
                !track.Name.empty()
                    ? track.Name
                    : fmt::format("{} {}", track.Kind == TrackKind::Virtual ? "Track" : "Thread",
                                  track.Id);
            emitThreadMeta(tid, name, SortIndex(track.Kind, track.Role, track.Id));
        }

        // ---- The frame track -------------------------------------------------------------------
        // Each frame is a complete event spanning the extent of every event that measures it, so a
        // back-dated GPU pass (an earlier frame than the one it was read in) sits under the frame it
        // measured. On a truncated capture the trailing frame is still open — its true end is past
        // the cut — so it is emitted as a bare B (running to the end of trace) rather than a span.
        {
            struct Extent
            {
                u64 MinBegin = std::numeric_limits<u64>::max();
                u64 MaxEnd = 0;
            };
            std::map<u64, Extent> frames;
            for (const Event& event : trace.Events)
            {
                Extent& extent = frames[event.Frame];
                extent.MinBegin = std::min(extent.MinBegin, event.BeginTicks);
                extent.MaxEnd = std::max(extent.MaxEnd, event.EndTicks);
            }

            const bool haveOpenFrame = trace.Truncated && !frames.empty();
            const u64 openFrame = haveOpenFrame ? frames.rbegin()->first : 0;

            for (const auto& [frame, extent] : frames)
            {
                Json frameEvent;
                frameEvent["name"] = fmt::format("Frame {}", frame);
                frameEvent["pid"] = ProcessId;
                frameEvent["tid"] = FrameTid;
                frameEvent["ts"] = toMicros(extent.MinBegin);
                frameEvent["args"] = Json{{"frame", frame}};
                if (haveOpenFrame && frame == openFrame)
                {
                    frameEvent["ph"] = "B";
                }
                else
                {
                    frameEvent["ph"] = "X";
                    frameEvent["dur"] = durMicros(extent.MinBegin, extent.MaxEnd);
                }
                events.push_back(std::move(frameEvent));
            }
        }

        // ---- The recorded events ---------------------------------------------------------------
        for (const Event& event : trace.Events)
        {
            const i64 tid =
                event.HasVirtualTrack ? VirtualTid(event.VirtualTrack) : ThreadTid(event.Thread);
            const string name = EventName(trace, event);

            switch (event.Type)
            {
            case RecordType::ScopeComplete:
                if (options.Events == EventForm::Pair)
                {
                    Json begin;
                    begin["ph"] = "B";
                    begin["name"] = name;
                    begin["pid"] = ProcessId;
                    begin["tid"] = tid;
                    begin["ts"] = toMicros(event.BeginTicks);
                    begin["args"] = Json{{"frame", event.Frame}};
                    events.push_back(std::move(begin));

                    Json end;
                    end["ph"] = "E";
                    end["pid"] = ProcessId;
                    end["tid"] = tid;
                    end["ts"] = toMicros(event.EndTicks);
                    events.push_back(std::move(end));
                }
                else
                {
                    Json span;
                    span["ph"] = "X";
                    span["name"] = name;
                    span["pid"] = ProcessId;
                    span["tid"] = tid;
                    span["ts"] = toMicros(event.BeginTicks);
                    span["dur"] = durMicros(event.BeginTicks, event.EndTicks);
                    span["args"] = Json{{"frame", event.Frame}};
                    events.push_back(std::move(span));
                }
                break;
            case RecordType::Counter:
            {
                // A counter carries only its own value series; a frame arg would render as a second,
                // spurious counter line in Perfetto's counter track.
                const string series = name.empty() ? string("value") : name;
                Json counter;
                counter["ph"] = "C";
                counter["name"] = name;
                counter["pid"] = ProcessId;
                counter["tid"] = tid;
                counter["ts"] = toMicros(event.BeginTicks);
                counter["args"] = Json{{series, CounterValue(event)}};
                events.push_back(std::move(counter));
                break;
            }
            case RecordType::Instant:
            {
                Json instant;
                instant["ph"] = "i";
                instant["name"] = name;
                instant["pid"] = ProcessId;
                instant["tid"] = tid;
                instant["ts"] = toMicros(event.BeginTicks);
                instant["s"] = "t"; // thread-scoped: the marker sits on the track that raised it.
                instant["args"] = Json{{"frame", event.Frame}};
                events.push_back(std::move(instant));
                break;
            }
            }
        }

        // ---- The document ----------------------------------------------------------------------
        Json other;
        other["producer"] = "vengtrace";
        other["note"] =
            "Lossy viewer-facing projection of a veng binary capture. The binary is the "
            "native form; nothing in veng or viz reads this JSON.";
        other["formatVersion"] = trace.FormatVersion;
        other["tickFrequency"] = trace.TickFrequency;
        other["captureMode"] = (trace.Mode == CaptureMode::RingDump) ? "ring" : "triggered";
        other["buildConfig"] = (trace.Config == BuildConfig::Release) ? "release" : "debug";
        other["profileEnabled"] = trace.ProfileEnabled;
        other["complete"] = trace.Complete;
        other["truncated"] = trace.Truncated;
        if (trace.HasAccounting)
        {
            other["droppedEvents"] = trace.DroppedEvents;
            other["droppedThreads"] = trace.DroppedThreads;
        }
        if (trace.Meta)
        {
            other["engineVersion"] = trace.Meta->EngineVersion;
            other["executable"] = trace.Meta->ExecutableBasename;
            Json submodules = Json::array();
            for (const Provenance& entry : trace.Meta->Submodules)
            {
                submodules.push_back(
                    Json{{"name", entry.Name}, {"sha", entry.ShortSha}, {"dirty", entry.Dirty}});
            }
            other["submodules"] = std::move(submodules);
        }
        if (!trace.UnknownSections.empty())
        {
            other["unknownSections"] = trace.UnknownSections;
        }

        Json document;
        document["traceEvents"] = std::move(events);
        document["displayTimeUnit"] = "ms";
        document["otherData"] = std::move(other);

        return options.Pretty ? document.dump(2) : document.dump();
    }
}
