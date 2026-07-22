#include "ProfileTools.h"

#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpTool.h>

#include <Veng/Diagnostics/Profiler.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <thread>

namespace Veng::Mcp
{
    using Json = nlohmann::json;
    using Diagnostics::CaptureState;
    using Diagnostics::CaptureStatus;
    using Diagnostics::Profiler;

    namespace
    {
        /// @brief Resolves the profiler through the host, or null when the seam is unset/unsupported.
        Profiler* ResolveProfiler(const McpHost& host)
        {
            return host.Profiler ? host.Profiler() : nullptr;
        }

        /// @brief The error a verb returns when no profiler is reachable through the host.
        Result<string> ProfilerUnavailable()
        {
            return std::unexpected(
                string("the profiler is unavailable: this host exposes no profiler"));
        }

        /// @brief Reads the optional `name` argument, defaulting to a stable capture name.
        string CaptureName(const Json& args)
        {
            if (args.is_object() && args.contains("name") && args["name"].is_string())
            {
                const string name = args["name"].get<string>();
                if (!name.empty())
                {
                    return name;
                }
            }
            return "capture";
        }

        /// @brief The status enumerator as a lowercase string, for a tool response.
        const char* StatusName(CaptureStatus status)
        {
            switch (status)
            {
            case CaptureStatus::Off:
                return "off";
            case CaptureStatus::Ring:
                return "ring";
            case CaptureStatus::Capturing:
                return "capturing";
            }
            return "off";
        }

        /// @brief Serializes a capture state to a JSON object for a tool response.
        Json StateJson(const CaptureState& state)
        {
            Json out{{"status", StatusName(state.Status)},
                     {"frame_budget", state.FrameBudget},
                     {"frames_elapsed", state.FramesElapsed},
                     {"writer_draining", state.WriterDraining}};
            if (!state.Path.empty())
            {
                out["path"] = state.Path.string();
            }
            return out;
        }

        /// @brief Blocks the handler until the off-thread writer has committed the last capture/dump.
        ///
        /// EndCapture and DumpRing return the path immediately and finish the file off-thread; a tool
        /// that must hand an agent a path it can act on waits on GetState().WriterDraining rather than
        /// on a frame. Bounded well under the server's request window so a stuck writer surfaces as a
        /// timeout rather than hanging the pump.
        void WaitForWriter(Profiler& profiler)
        {
            for (int i = 0; i < 400; ++i)
            {
                if (!profiler.GetState().WriterDraining)
                {
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }

    void RegisterProfileReadTools(McpServer& server, const McpHost& host)
    {
        // profile.stats — the live per-scope aggregates plus the profiler state and drop counters.
        {
            McpTool tool;
            tool.Name = "profile.stats";
            tool.Description =
                "Reports the profiler's live per-scope aggregates for the last completed frame "
                "(inclusive/self nanoseconds and call count per named scope), the current capture "
                "state (off/ring/capturing), and the drop counters. Read-only: reads nothing off "
                "disk and starts no capture.";
            tool.InputSchemaJson = R"({"type":"object","properties":{}})";
            tool.Handler = [&host](string_view) -> Result<string>
            {
                Profiler* const profiler = ResolveProfiler(host);
                if (profiler == nullptr)
                {
                    return ProfilerUnavailable();
                }

                Json scopes = Json::array();
                for (const Diagnostics::ScopeAggregate& aggregate : profiler->GetFrameAggregates())
                {
                    scopes.push_back(Json{{"name", string(profiler->GetName(aggregate.Name))},
                                          {"calls", aggregate.CallCount},
                                          {"inclusive_ns", aggregate.InclusiveNanos},
                                          {"self_ns", aggregate.SelfNanos},
                                          {"last_active_frame", aggregate.LastActiveFrame}});
                }

                Json out = StateJson(profiler->GetState());
                out["frame_index"] = profiler->GetFrameIndex();
                out["dropped_events"] = profiler->GetDroppedEventCount();
                out["dropped_threads"] = profiler->GetRegistrationOverflowCount();
                out["scopes"] = std::move(scopes);
                return out.dump();
            };
            server.RegisterTool(std::move(tool));
        }
    }

    void RegisterProfileWriteTools(McpServer& server, const McpHost& host)
    {
        // profile.start — begin a capture, self-terminating after `frames` when given. Returns
        // immediately with the state and the planned path; it never blocks for the frame budget.
        {
            McpTool tool;
            tool.Name = "profile.start";
            tool.Description =
                "Begins a profiler capture, streaming to a trace file. Optional 'frames' "
                "self-terminates the capture that many frames later (the form to use over MCP — "
                "the "
                "call returns immediately, never waiting for the frames). Optional 'name' names "
                "the "
                "capture; the engine resolves it under the capture directory (no path argument). "
                "Returns the capture state and the planned path. End an open-ended capture with "
                "profile.stop.";
            tool.InputSchemaJson =
                R"({"type":"object","properties":{"frames":{"type":"integer","minimum":0},"name":{"type":"string"}}})";
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                Profiler* const profiler = ResolveProfiler(host);
                if (profiler == nullptr)
                {
                    return ProfilerUnavailable();
                }

                const Json args = Json::parse(argsJson, nullptr, false);
                u64 frames = 0;
                if (args.is_object() && args.contains("frames") &&
                    args["frames"].is_number_integer())
                {
                    const i64 value = args["frames"].get<i64>();
                    if (value < 0)
                    {
                        return std::unexpected(string("'frames' must be non-negative"));
                    }
                    frames = static_cast<u64>(value);
                }

                const path file = Diagnostics::ResolveCapturePath(CaptureName(args));
                const VoidResult begun = profiler->BeginCapture(file, frames);
                if (!begun)
                {
                    return std::unexpected(begun.error());
                }
                return StateJson(profiler->GetState()).dump();
            };
            server.RegisterTool(std::move(tool));
        }

        // profile.stop — end the running capture and return the written path once it is on disk.
        {
            McpTool tool;
            tool.Name = "profile.stop";
            tool.Description =
                "Ends the running profiler capture and returns the path of the written trace file, "
                "waiting until the file is committed to disk. Fails if no capture is running.";
            tool.InputSchemaJson = R"({"type":"object","properties":{}})";
            tool.Handler = [&host](string_view) -> Result<string>
            {
                Profiler* const profiler = ResolveProfiler(host);
                if (profiler == nullptr)
                {
                    return ProfilerUnavailable();
                }

                const Result<path> ended = profiler->EndCapture();
                if (!ended)
                {
                    return std::unexpected(ended.error());
                }
                WaitForWriter(*profiler);
                return Json{{"path", ended.value().string()}}.dump();
            };
            server.RegisterTool(std::move(tool));
        }

        // profile.dump_ring — freeze and dump the retained ring, returning the written path.
        {
            McpTool tool;
            tool.Name = "profile.dump_ring";
            tool.Description =
                "Dumps the continuous ring (the last N seconds retained in memory) to a trace file "
                "and returns its path, waiting until the file is committed. Optional 'name' names "
                "the capture; the engine resolves it under the capture directory (no path "
                "argument). "
                "The tool to reach for after something looked wrong. Fails if a capture is "
                "running.";
            tool.InputSchemaJson = R"({"type":"object","properties":{"name":{"type":"string"}}})";
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                Profiler* const profiler = ResolveProfiler(host);
                if (profiler == nullptr)
                {
                    return ProfilerUnavailable();
                }

                const Json args = Json::parse(argsJson, nullptr, false);
                const path file = Diagnostics::ResolveCapturePath(CaptureName(args));
                const Result<path> dumped = profiler->DumpRing(file);
                if (!dumped)
                {
                    return std::unexpected(dumped.error());
                }
                WaitForWriter(*profiler);
                return Json{{"path", dumped.value().string()}}.dump();
            };
            server.RegisterTool(std::move(tool));
        }
    }
}
