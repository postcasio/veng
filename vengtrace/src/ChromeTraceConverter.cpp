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

        // The category an async slice carries, so a lane's slices are queryable by the kind of work
        // they measure.
        const char* RoleCategory(TrackRole role)
        {
            switch (role)
            {
            case TrackRole::Gpu:
                return "gpu";
            case TrackRole::Custom:
                return "custom";
            case TrackRole::Cpu:
                break;
            }
            return "cpu";
        }

        // One packed async slice.
        struct AsyncSlice
        {
            u64 BeginTicks = 0;
            u64 EndTicks = 0;
            i64 Cookie = 0;
            i64 Tid = 0;
            u64 Frame = 0;
            // The index into DecodedTrace::Events this slice came from; meaningless when IsFrame,
            // since a frame-ruler slice is synthesized from a frame's extent rather than recorded.
            usize Event = 0;
            bool IsFrame = false;
            // A truncated capture's trailing frame, whose true end is past the cut: it is emitted as
            // a begin with no end and runs to the end of the trace.
            bool Open = false;
        };

        // One endpoint of an async slice: the begin or the end the converter writes out.
        struct AsyncEndpoint
        {
            u64 Ticks = 0;
            usize Slice = 0;
            bool IsEnd = false;
        };

        // Packs spans onto async cookies so that every span sharing a cookie is properly nested.
        //
        // A complete (X) event carries no parenthood, so a viewer can only infer nesting from
        // timestamp containment — which is ambiguous the moment two spans on one lane partially
        // overlap. An async cookie states the parenthood instead: a span contained by a lane's
        // innermost open span nests on that lane's cookie, and a span that merely straddles it takes
        // a different lane, so overlap between cookies is legal and never has to be guessed.
        //
        // The packer also writes the endpoints, because their order carries meaning a timestamp
        // cannot. A viewer sorts a trace by timestamp and breaks a tie by document order, so where
        // two endpoints on one cookie share a timestamp — a child closing with its parent, a span
        // beginning exactly where the previous one ended, a zero-duration span — document order
        // decides which begin an end closes. Writing each endpoint at the moment the packer opens or
        // closes it produces the one order that reads back as the nesting the packer chose.
        //
        // Spans must be offered in non-decreasing begin order, ties outermost-first.
        class CookiePacker
        {
        public:
            /// @brief Constructs a packer over the document's cookie counter and slice tables.
            /// @param nextCookie  The counter, advanced once per lane this packer opens.
            /// @param slices      The document's slice table, appended to.
            /// @param endpoints   The document's endpoint stream, appended to.
            CookiePacker(i64& nextCookie, vector<AsyncSlice>& slices,
                         vector<AsyncEndpoint>& endpoints)
                : m_NextCookie(nextCookie), m_Slices(slices), m_Endpoints(endpoints)
            {
            }

            /// @brief Places a span on a lane it nests cleanly on and opens it.
            /// @param slice  The slice, with its cookie unset.
            void Add(AsyncSlice slice)
            {
                const usize laneCount = m_Lanes.size();
                usize chosen = laneCount;
                usize vacant = laneCount;
                u64 tightest = std::numeric_limits<u64>::max();
                for (usize lane = 0; lane < laneCount; ++lane)
                {
                    while (!m_Lanes[lane].Open.empty() &&
                           EndOfTop(m_Lanes[lane]) <= slice.BeginTicks)
                    {
                        Close(m_Lanes[lane]);
                    }
                    if (m_Lanes[lane].Open.empty())
                    {
                        vacant = std::min(vacant, lane);
                    }
                    else if (EndOfTop(m_Lanes[lane]) >= slice.EndTicks &&
                             EndOfTop(m_Lanes[lane]) < tightest)
                    {
                        // The tightest containing lane is the truest parent available.
                        tightest = EndOfTop(m_Lanes[lane]);
                        chosen = lane;
                    }
                }
                if (chosen == laneCount)
                {
                    chosen = vacant;
                }
                if (chosen == laneCount)
                {
                    m_Lanes.push_back(Lane{.Cookie = m_NextCookie++});
                }

                slice.Cookie = m_Lanes[chosen].Cookie;
                const usize index = m_Slices.size();
                m_Slices.push_back(slice);
                m_Endpoints.push_back(
                    AsyncEndpoint{.Ticks = slice.BeginTicks, .Slice = index, .IsEnd = false});
                m_Lanes[chosen].Open.push_back(index);
            }

            /// @brief Closes every span still open, innermost first.
            void Flush()
            {
                for (Lane& lane : m_Lanes)
                {
                    while (!lane.Open.empty())
                    {
                        Close(lane);
                    }
                }
            }

        private:
            struct Lane
            {
                i64 Cookie = 0;
                vector<usize> Open;
            };

            [[nodiscard]] u64 EndOfTop(const Lane& lane) const
            {
                return m_Slices[lane.Open.back()].EndTicks;
            }

            void Close(Lane& lane)
            {
                const usize index = lane.Open.back();
                lane.Open.pop_back();
                if (!m_Slices[index].Open)
                {
                    m_Endpoints.push_back(AsyncEndpoint{
                        .Ticks = m_Slices[index].EndTicks, .Slice = index, .IsEnd = true});
                }
            }

            vector<Lane> m_Lanes;
            i64& m_NextCookie;
            vector<AsyncSlice>& m_Slices;
            vector<AsyncEndpoint>& m_Endpoints;
        };

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

        // Cookies are document-wide: Chrome Trace scopes an unqualified async id globally, so no two
        // lanes may share one.
        i64 nextCookie = 0;
        vector<AsyncSlice> asyncSlices;
        vector<AsyncEndpoint> asyncEndpoints;

        // ---- The frame track -------------------------------------------------------------------
        // Each frame spans the extent of every event that measures it, so a back-dated GPU pass (an
        // earlier frame than the one it was read in) sits under the frame it measured. Pipelining
        // makes consecutive frames overlap by construction — frame N's GPU work finishes after frame
        // N+1's CPU work began — so frames are async slices, each with its own cookie. On a
        // truncated capture the trailing frame is still open (its true end is past the cut), so it
        // is emitted as a begin with no end, running to the end of the trace.
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

            // The packer wants begin order, which frame-number order need not be.
            vector<std::pair<u64, Extent>> ordered(frames.begin(), frames.end());
            std::ranges::stable_sort(ordered,
                                     [](const auto& left, const auto& right)
                                     {
                                         if (left.second.MinBegin != right.second.MinBegin)
                                         {
                                             return left.second.MinBegin < right.second.MinBegin;
                                         }
                                         return left.second.MaxEnd > right.second.MaxEnd;
                                     });

            CookiePacker packer(nextCookie, asyncSlices, asyncEndpoints);
            for (const auto& [frame, extent] : ordered)
            {
                packer.Add(AsyncSlice{.BeginTicks = extent.MinBegin,
                                      .EndTicks = extent.MaxEnd,
                                      .Tid = FrameTid,
                                      .Frame = frame,
                                      .IsFrame = true,
                                      .Open = haveOpenFrame && frame == openFrame});
            }
            packer.Flush();
        }

        // ---- Async cookies for the virtual lanes -----------------------------------------------
        // A virtual lane carries spans a bridge back-dated onto it from hardware that ran them
        // concurrently: consecutive frames' work overlaps, and sibling passes with no barrier
        // between them genuinely interleave. Containment therefore does not hold across a virtual
        // lane, so its spans are async slices whose cookies state the nesting outright. A thread
        // lane is the opposite case — scoped timing nests by construction — and stays on complete
        // events.
        vector<bool> isAsync(trace.Events.size(), false);
        {
            std::map<u32, vector<usize>> byTrack;
            for (usize index = 0; index < trace.Events.size(); ++index)
            {
                const Event& event = trace.Events[index];
                if (event.Type == RecordType::ScopeComplete && event.HasVirtualTrack)
                {
                    byTrack[event.VirtualTrack].push_back(index);
                }
            }
            for (auto& [track, indices] : byTrack)
            {
                std::ranges::stable_sort(indices,
                                         [&trace](usize left, usize right)
                                         {
                                             const Event& a = trace.Events[left];
                                             const Event& b = trace.Events[right];
                                             if (a.BeginTicks != b.BeginTicks)
                                             {
                                                 return a.BeginTicks < b.BeginTicks;
                                             }
                                             return a.EndTicks > b.EndTicks;
                                         });
                CookiePacker packer(nextCookie, asyncSlices, asyncEndpoints);
                for (const usize index : indices)
                {
                    const Event& event = trace.Events[index];
                    isAsync[index] = true;
                    packer.Add(AsyncSlice{.BeginTicks = event.BeginTicks,
                                          .EndTicks = event.EndTicks,
                                          .Tid = VirtualTid(event.VirtualTrack),
                                          .Frame = event.Frame,
                                          .Event = index});
                }
                packer.Flush();
            }
        }

        // A virtual lane's role, for the category its async slices carry.
        std::map<u32, TrackRole> virtualRoles;
        for (const Track& track : trace.Tracks)
        {
            if (track.Kind == TrackKind::Virtual)
            {
                virtualRoles[track.Id] = track.Role;
            }
        }

        // ---- The async slices ------------------------------------------------------------------
        // The stream is sorted by timestamp alone, and stably: the packer already wrote each
        // cookie's endpoints in the order they must be read, and a stable sort keeps that order
        // wherever timestamps tie.
        {
            std::ranges::stable_sort(asyncEndpoints,
                                     [](const AsyncEndpoint& left, const AsyncEndpoint& right)
                                     { return left.Ticks < right.Ticks; });

            for (const AsyncEndpoint& endpoint : asyncEndpoints)
            {
                const AsyncSlice& slice = asyncSlices[endpoint.Slice];
                const Event* event = slice.IsFrame ? nullptr : &trace.Events[slice.Event];
                const string name =
                    slice.IsFrame ? fmt::format("Frame {}", slice.Frame) : EventName(trace, *event);
                const char* category = "frame";
                if (event != nullptr)
                {
                    const auto role = virtualRoles.find(event->VirtualTrack);
                    category = RoleCategory((role != virtualRoles.end()) ? role->second
                                                                         : TrackRole::Custom);
                }

                // Perfetto matches an end to its begin by name and category as well as by id, so the
                // end repeats both.
                Json async;
                async["ph"] = endpoint.IsEnd ? "e" : "b";
                async["name"] = name;
                async["cat"] = category;
                async["id"] = slice.Cookie;
                async["pid"] = ProcessId;
                async["tid"] = slice.Tid;
                async["ts"] = toMicros(endpoint.Ticks);
                if (!endpoint.IsEnd)
                {
                    async["args"] = Json{{"frame", slice.Frame}};
                }
                events.push_back(std::move(async));
            }
        }

        // ---- The recorded events ---------------------------------------------------------------
        for (usize index = 0; index < trace.Events.size(); ++index)
        {
            const Event& event = trace.Events[index];
            const i64 tid =
                event.HasVirtualTrack ? VirtualTid(event.VirtualTrack) : ThreadTid(event.Thread);
            const string name = EventName(trace, event);

            switch (event.Type)
            {
            case RecordType::ScopeComplete:
                if (isAsync[index])
                {
                    break; // already emitted as an async slice
                }
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
