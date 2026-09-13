#include "RenderTools.h"

#include "ViewportCapture.h"

#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpTool.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Capture/VideoRecorder.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/FormatInfo.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Viewport.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <span>

namespace Veng::Mcp
{
    using Json = nlohmann::json;

    namespace
    {
        /// @brief Reads the optional `viewport` name argument, defaulting to the empty (primary) name.
        string ViewportName(const Json& args)
        {
            if (args.is_object() && args.contains("viewport") && args["viewport"].is_string())
            {
                return args["viewport"].get<string>();
            }
            return {};
        }

        /// @brief Resolves a viewport by name through the host, or null when unset/unknown.
        Renderer::Viewport* ResolveViewport(const McpHost& host, const string& name)
        {
            return host.Viewport ? host.Viewport(name) : nullptr;
        }

        /// @brief Resolves the video recorder through the host, or null when the seam is unset.
        Capture::VideoRecorder* ResolveRecorder(const McpHost& host)
        {
            return host.VideoRecorder ? host.VideoRecorder() : nullptr;
        }

        /// @brief The reason a host with no reachable recorder gives every capture verb.
        constexpr string_view RecorderUnavailable =
            "video capture is unavailable: this host exposes no recorder";

        /// @brief The capture status as the enumerator's own name, for a tool response.
        const char* StatusName(const Capture::VideoCaptureStatus status)
        {
            switch (status)
            {
            case Capture::VideoCaptureStatus::Off:
                return "Off";
            case Capture::VideoCaptureStatus::Recording:
                return "Recording";
            case Capture::VideoCaptureStatus::Finalizing:
                return "Finalizing";
            }
            return "Off";
        }

        /// @brief The resolved encoding as the enumerator's own name, for a tool response.
        const char* EncodingName(const Capture::CaptureEncoding encoding)
        {
            switch (encoding)
            {
            case Capture::CaptureEncoding::Auto:
                return "Auto";
            case Capture::CaptureEncoding::Sdr:
                return "Sdr";
            case Capture::CaptureEncoding::Hdr10:
                return "Hdr10";
            }
            return "Sdr";
        }

        /// @brief Serializes a capture state, plus whether this run can record at all.
        Json CaptureStateJson(const Capture::VideoCaptureState& state, const bool available)
        {
            return Json{{"available", available},
                        {"status", StatusName(state.Status)},
                        {"lockstep", state.Lockstep},
                        {"encoding", EncodingName(state.Encoding)},
                        {"include_overlay", state.IncludeOverlay},
                        {"codec", state.Codec},
                        {"bitrate_mbps", state.BitrateMbps},
                        {"extent", Json::array({state.Extent.x, state.Extent.y})},
                        {"frames_acquired", state.FramesAcquired},
                        {"frames_appended", state.FramesAppended},
                        {"frame_budget", state.FrameBudget},
                        {"duration_seconds", state.DurationSeconds},
                        {"frames_waited", state.FramesWaited},
                        {"waited_for_encoder_ms", state.WaitedForEncoderMs},
                        {"audio_blocks", state.AudioBlocks},
                        {"audio_overruns", state.AudioOverruns},
                        {"bytes_written", state.BytesWritten},
                        {"path", state.Path.string()},
                        {"last_error", state.LastError}};
        }

        /// @brief Resolves one enumerator name out of @p names, case-insensitively.
        /// @param text   The requested name.
        /// @param names  The enumerator names, in enumerator order.
        /// @return The enumerator's index, or nullopt when no name matches.
        optional<usize> FindEnumerator(const string& text, std::span<const string_view> names)
        {
            for (usize index = 0; index < names.size(); ++index)
            {
                if (std::ranges::equal(text, names[index], [](const char a, const char b)
                                       { return std::tolower(a) == std::tolower(b); }))
                {
                    return index;
                }
            }
            return std::nullopt;
        }

        /// @brief The encoding arms a capture_start argument may name.
        constexpr std::array<string_view, 3> EncodingNames{"Auto", "Sdr", "Hdr10"};
        /// @brief The codec arms a capture_start argument may name.
        constexpr std::array<string_view, 4> CodecNames{"Hevc", "H264", "ProRes422HQ",
                                                        "ProRes4444"};
        /// @brief The sound-track arms a capture_start argument may name.
        constexpr std::array<string_view, 3> AudioNames{"None", "Pcm", "Aac"};
        /// @brief Every key capture_start accepts; anything else is rejected as unknown.
        constexpr std::array<string_view, 10> SettingKeys{
            "encoding", "codec",           "bits_per_pixel", "bitrate_mbps", "lockstep",
            "name",     "include_overlay", "frame_rate",     "frame_budget", "audio"};

        /// @brief Parses and validates a capture_start argument object into settings.
        ///
        /// The server validates nothing against a tool's schema, so every rule lives here: an
        /// unknown key (a directory among them — a capture is named, never pathed), an unknown
        /// enumerator, a non-positive frame rate, a quality that derives no bitrate, and a codec
        /// that cannot carry the requested encoding are each a tool error rather than a silently
        /// ignored field.
        /// @param args  The tools/call arguments object.
        /// @return The settings, or the reason they were refused.
        Result<Capture::VideoCaptureSettings> ParseCaptureSettings(const Json& args)
        {
            Capture::VideoCaptureSettings settings;
            if (!args.is_object())
            {
                return settings;
            }

            for (const auto& [key, value] : args.items())
            {
                if (std::ranges::find(SettingKeys, key) == SettingKeys.end())
                {
                    return std::unexpected(fmt::format(
                        "unknown setting '{}'; a capture is named, never pathed — the engine "
                        "resolves 'name' under the capture directory",
                        key));
                }
            }

            const auto enumArg = [&args](const string& key, std::span<const string_view> names,
                                         const string& choices) -> Result<optional<usize>>
            {
                if (!args.contains(key))
                {
                    return optional<usize>{};
                }
                const Json& value = args[key];
                if (!value.is_string())
                {
                    return std::unexpected(
                        fmt::format("'{}' names one of {} as a string", key, choices));
                }
                const optional<usize> index = FindEnumerator(value.get<string>(), names);
                if (!index)
                {
                    return std::unexpected(fmt::format("unknown {} '{}'; one of {}", key,
                                                       value.get<string>(), choices));
                }
                return index;
            };

            const auto intArg = [&args](const string& key, const i64 least) -> Result<optional<i64>>
            {
                if (!args.contains(key))
                {
                    return optional<i64>{};
                }
                const Json& value = args[key];
                if (!value.is_number_integer())
                {
                    return std::unexpected(fmt::format("'{}' is an integer", key));
                }
                const i64 number = value.get<i64>();
                if (number < least)
                {
                    return std::unexpected(fmt::format("'{}' must be at least {}", key, least));
                }
                return number;
            };

            const Result<optional<usize>> encoding =
                enumArg("encoding", EncodingNames, "Auto, Sdr, Hdr10");
            if (!encoding)
            {
                return std::unexpected(encoding.error());
            }
            if (*encoding)
            {
                settings.Encoding = static_cast<Capture::CaptureEncoding>(**encoding);
            }

            const Result<optional<usize>> codec =
                enumArg("codec", CodecNames, "Hevc, H264, ProRes422HQ, ProRes4444");
            if (!codec)
            {
                return std::unexpected(codec.error());
            }
            if (*codec)
            {
                settings.Codec = static_cast<Capture::VideoCodec>(**codec);
            }

            const Result<optional<usize>> audio = enumArg("audio", AudioNames, "None, Pcm, Aac");
            if (!audio)
            {
                return std::unexpected(audio.error());
            }
            if (*audio)
            {
                settings.Audio = static_cast<Capture::AudioTrack>(**audio);
            }

            const Result<optional<i64>> bitrate = intArg("bitrate_mbps", 0);
            if (!bitrate)
            {
                return std::unexpected(bitrate.error());
            }
            if (*bitrate)
            {
                settings.BitrateMbps = static_cast<u32>(**bitrate);
            }

            const Result<optional<i64>> frameRate = intArg("frame_rate", 1);
            if (!frameRate)
            {
                return std::unexpected(frameRate.error());
            }
            if (*frameRate)
            {
                settings.FrameRate = static_cast<u32>(**frameRate);
            }

            const Result<optional<i64>> budget = intArg("frame_budget", 0);
            if (!budget)
            {
                return std::unexpected(budget.error());
            }
            if (*budget)
            {
                settings.FrameBudget = static_cast<u64>(**budget);
            }

            if (args.contains("bits_per_pixel"))
            {
                if (!args["bits_per_pixel"].is_number())
                {
                    return std::unexpected(string("'bits_per_pixel' is a number"));
                }
                settings.BitsPerPixel = args["bits_per_pixel"].get<f32>();
            }
            if (args.contains("lockstep"))
            {
                if (!args["lockstep"].is_boolean())
                {
                    return std::unexpected(string("'lockstep' is a boolean"));
                }
                settings.Lockstep = args["lockstep"].get<bool>();
            }
            if (args.contains("include_overlay"))
            {
                if (!args["include_overlay"].is_boolean())
                {
                    return std::unexpected(string("'include_overlay' is a boolean"));
                }
                settings.IncludeOverlay = args["include_overlay"].get<bool>();
            }
            if (args.contains("name"))
            {
                if (!args["name"].is_string())
                {
                    return std::unexpected(string("'name' is a string"));
                }
                settings.Name = args["name"].get<string>();
            }

            if (settings.BitrateMbps == 0 && settings.BitsPerPixel <= 0.0f)
            {
                return std::unexpected(
                    string("'bits_per_pixel' must be positive, or 'bitrate_mbps' must be given"));
            }
            if (!Capture::CodecSupportsEncoding(settings.Codec, settings.Encoding))
            {
                return std::unexpected(fmt::format("{} cannot encode {}",
                                                   CodecNames[static_cast<usize>(settings.Codec)],
                                                   EncodingName(settings.Encoding)));
            }
            return settings;
        }

        /// @brief Default page size for render.bindless_slots when the caller omits `limit`.
        ///
        /// A cap on how much of a 1024-slot array one call dumps into an agent's context; the
        /// caller pages the tail through nextCursor.
        constexpr u32 DefaultSlotLimit = 200;

        /// @brief Parses the optional `limit` argument, clamped to the page cap.
        u32 ParseSlotLimit(const Json& args)
        {
            if (!args.is_object() || !args.contains("limit") || !args["limit"].is_number())
            {
                return DefaultSlotLimit;
            }
            const i64 requested = args["limit"].get<i64>();
            if (requested <= 0)
            {
                return DefaultSlotLimit;
            }
            return static_cast<u32>(std::min<i64>(requested, DefaultSlotLimit));
        }

        /// @brief Parses the opaque `cursor` (the resume slot index), defaulting to 0.
        u32 ParseSlotCursor(const Json& args)
        {
            if (!args.is_object() || !args.contains("cursor"))
            {
                return 0;
            }
            const Json& cursor = args["cursor"];
            if (cursor.is_number())
            {
                const i64 value = cursor.get<i64>();
                return value < 0 ? 0 : static_cast<u32>(value);
            }
            if (cursor.is_string())
            {
                const string text = cursor.get<string>();
                u64 value = 0;
                if (std::from_chars(text.data(), text.data() + text.size(), value).ec ==
                    std::errc{})
                {
                    return static_cast<u32>(value);
                }
            }
            return 0;
        }

        /// @brief Which slot states a call reports: the `state` argument, defaulting to occupied.
        ///
        /// Occupied is the default because "what is in there" is the question the tool exists for;
        /// a free slot carries no description, so listing them is only useful for reading the
        /// fragmentation of the free list itself.
        struct SlotFilter
        {
            /// @brief Whether an occupied slot is reported.
            bool Occupied = true;
            /// @brief Whether a slot inside its deferred-release window is reported.
            bool PendingRelease = true;
            /// @brief Whether a free slot is reported.
            bool Free = false;
        };

        /// @brief Parses the optional `state` filter, or nothing when it names no known state.
        optional<SlotFilter> ParseSlotFilter(const Json& args)
        {
            if (!args.is_object() || !args.contains("state") || !args["state"].is_string())
            {
                return SlotFilter{};
            }
            const string state = args["state"].get<string>();
            if (state == "occupied")
            {
                return SlotFilter{};
            }
            if (state == "free")
            {
                return SlotFilter{.Occupied = false, .PendingRelease = false, .Free = true};
            }
            if (state == "pending_release")
            {
                return SlotFilter{.Occupied = false, .PendingRelease = true, .Free = false};
            }
            if (state == "all")
            {
                return SlotFilter{.Occupied = true, .PendingRelease = true, .Free = true};
            }
            return std::nullopt;
        }

        /// @brief Whether @p filter admits a slot in @p state.
        bool Admits(const SlotFilter& filter, const Renderer::BindlessSlotState state)
        {
            switch (state)
            {
            case Renderer::BindlessSlotState::Free:
                return filter.Free;
            case Renderer::BindlessSlotState::Occupied:
                return filter.Occupied;
            case Renderer::BindlessSlotState::PendingRelease:
                return filter.PendingRelease;
            }
            return false;
        }

        /// @brief One slot as the tool reports it, with the fields its array does not describe left
        ///        out rather than reported as zeros.
        Json DescribeSlot(const Renderer::BindlessArray array, const Renderer::BindlessSlot& slot)
        {
            Json out{{"index", slot.Index}, {"state", Renderer::BindlessSlotStateName(slot.State)}};
            if (!slot.Name.empty())
            {
                out["name"] = slot.Name;
            }
            if (slot.ImageFormat != Renderer::Format::Undefined)
            {
                out["format"] = Renderer::FormatName(slot.ImageFormat);
                out["extent"] = {slot.Extent.x, slot.Extent.y, slot.Extent.z};
                out["mips"] = slot.MipLevels;
                out["layers"] = slot.ArrayLayers;
                out["bytes"] = slot.ImageBytes;
            }
            else if (slot.SizeBytes != 0)
            {
                // The storage-buffer array reports the buffer's size; the material table reports
                // its cached block's length. Both are the slot's own byte count, so one key.
                out["bytes"] = slot.SizeBytes;
            }
            else if (array == Renderer::BindlessArray::Materials &&
                     slot.State != Renderer::BindlessSlotState::Free)
            {
                // A registered material whose block is empty is a real state, not a missing read.
                out["bytes"] = 0;
            }
            return out;
        }
    }

    void RegisterRenderTools(McpServer& server, const McpHost& host)
    {
        // render.screenshot — the tonemapped output of a viewport as a PNG image content block,
        // plus its pixel dimensions. A world's own GuiOverlay is driven onto the viewport's layer
        // stack, so it is part of that output; only the app's overlay sits outside it.
        {
            McpTool tool;
            tool.Name = "render.screenshot";
            tool.Description =
                "Captures a viewport's rendered output as a PNG image (tonemapped 8-bit), "
                "including any GuiOverlay a world drives into that viewport but not the app's "
                "own overlay. Optional 'viewport' names the viewport (default the primary). "
                "Returns an image content block; over the --connect CLI it requires --output "
                "<file> to write the PNG (an image is never printed to stdout).";
            tool.InputSchemaJson =
                R"({"type":"object","properties":{"viewport":{"type":"string"}}})";
            tool.ReturnsContentBlocks = true;
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                const Json args = Json::parse(argsJson, nullptr, false);
                const string name = ViewportName(args);
                Renderer::Viewport* viewport = ResolveViewport(host, name);
                if (viewport == nullptr)
                {
                    return std::unexpected(name.empty()
                                               ? string("no primary viewport is available")
                                               : fmt::format("no viewport named '{}'", name));
                }

                return CaptureViewportContentBlocks(*viewport);
            };
            server.RegisterTool(std::move(tool));
        }

        // render.screenshot_window — the presented frame, the only surface carrying the app's
        // own UI overlay.
        {
            // The capture reads the context's presented-frame mirror, so it must be armed before a
            // frame that is to be captured ends. Arming at registration — which precedes the first
            // Pump, and so the listener thread that could carry a call — means the first call
            // already finds a mirrored frame.
            if (Renderer::Context* const context =
                    host.RenderContext ? host.RenderContext() : nullptr)
            {
                context->ArmPresentedFrameCapture();
            }

            McpTool tool;
            tool.Name = "render.screenshot_window";
            tool.Description =
                "Captures the presented frame as a PNG image — the finished composite of the "
                "scene and the UI overlay drawn over it, which is what an app's interface looks "
                "like on screen. render.screenshot captures a viewport instead, which carries the "
                "scene and any GuiOverlay a world drives into it, but not the app's own overlay "
                "composited over the frame. Returns an image content block; over the --connect "
                "CLI it requires --output <file> to write the PNG (an image is never printed to "
                "stdout). Unavailable headless (no swap chain, and no UI overlay to capture) and "
                "where the surface did not grant transfer-source usage on its swap chain images.";
            tool.InputSchemaJson = R"({"type":"object","properties":{}})";
            tool.ReturnsContentBlocks = true;
            tool.Handler = [&host](string_view) -> Result<string>
            {
                Renderer::Context* const context =
                    host.RenderContext ? host.RenderContext() : nullptr;
                if (context == nullptr)
                {
                    return std::unexpected(
                        string("presented-frame capture is unavailable: this host exposes no "
                               "render context"));
                }
                return CaptureSwapChainContentBlocks(*context);
            };
            server.RegisterTool(std::move(tool));
        }

        // render.capture_status — what the recorder is doing, and whether this run can record at
        // all. Read-only, so it is registered whatever the server's write posture, and polling it
        // is how a caller learns a stopped capture has finished committing its file.
        {
            McpTool tool;
            tool.Name = "render.capture_status";
            tool.Description =
                "Reports the video recorder's state: 'available' (whether this build and run can "
                "record at all), 'status' (Off/Recording/Finalizing), the resolved encoding, "
                "codec and bitrate, the recorded extent, frames acquired and appended against the "
                "frame budget, file duration and size, the encoder-wait and audio-loss counters, "
                "the file's path, and the reason the last capture refused or ended early. "
                "Read-only. Stop returns as soon as the stop is requested, so a caller that needs "
                "the file finished polls this until 'status' is Off.";
            tool.InputSchemaJson = R"({"type":"object","properties":{}})";
            tool.Handler = [&host](string_view) -> Result<string>
            {
                Capture::VideoRecorder* const recorder = ResolveRecorder(host);
                if (recorder == nullptr)
                {
                    Json out = CaptureStateJson(Capture::VideoCaptureState{}, false);
                    out["reason"] = string(RecorderUnavailable);
                    return out.dump();
                }
                return CaptureStateJson(recorder->GetState(), recorder->IsAvailable()).dump();
            };
            server.RegisterTool(std::move(tool));
        }

        // render.list_viewports — the viewports the host chose to expose, each with its region
        // extent and role where resolvable.
        {
            McpTool tool;
            tool.Name = "render.list_viewports";
            tool.Description = "Lists the viewports the host exposes: each name plus its region "
                               "extent and role (Presented/Offscreen) where resolvable.";
            tool.InputSchemaJson = R"({"type":"object","properties":{}})";
            tool.Handler = [&host](string_view) -> Result<string>
            {
                Json viewports = Json::array();
                const vector<string> names =
                    host.ViewportNames ? host.ViewportNames() : vector<string>{};
                for (const string& name : names)
                {
                    Json item{{"name", name}};
                    if (Renderer::Viewport* viewport = ResolveViewport(host, name))
                    {
                        const Renderer::ViewportRegion& region = viewport->GetRegion();
                        item["extent"] = Json::array({region.Extent.x, region.Extent.y});
                        item["role"] = viewport->GetRole() == Renderer::ViewportRole::Presented
                                           ? "Presented"
                                           : "Offscreen";
                    }
                    viewports.push_back(std::move(item));
                }
                return Json{{"viewports", std::move(viewports)}}.dump();
            };
            server.RegisterTool(std::move(tool));
        }

        // render.stats — a viewport's cull funnel and the completed-frame GPU time.
        {
            McpTool tool;
            tool.Name = "render.stats";
            tool.Description =
                "Reports a viewport's cull funnel (visible/frustum_survived/drawn/gpu_survivors, "
                "broadphase state), the last completed-frame GPU time in milliseconds, and its "
                "render-scale state: render_scale (the effective current scale dynamic resolution "
                "drives), allocation_scale (the ceiling the render target is sized to; the "
                "sub-rect "
                "fraction rendered inside it is render_scale / allocation_scale), "
                "allocation_extent (the render-target pixels, distinct from the full-window region "
                "render.list_viewports reports), and dynamic_resolution — with drs_min_scale / "
                "drs_max_scale / drs_target_frame_ms when it is on. Optional 'viewport' names the "
                "viewport (default the primary).";
            tool.InputSchemaJson =
                R"({"type":"object","properties":{"viewport":{"type":"string"}}})";
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                const Json args = Json::parse(argsJson, nullptr, false);
                const string name = ViewportName(args);
                Renderer::Viewport* viewport = ResolveViewport(host, name);
                if (viewport == nullptr)
                {
                    return std::unexpected(name.empty()
                                               ? string("no primary viewport is available")
                                               : fmt::format("no viewport named '{}'", name));
                }

                const Renderer::SceneRenderer& renderer = viewport->GetRenderer();
                const uvec2 alloc = viewport->GetAllocationExtent();
                const uvec2 valid = renderer.GetValidExtent();
                const Renderer::SceneRendererSettings& s = renderer.GetSettings();
                const bool cullGpu =
                    renderer.GetActiveCullMode() == Renderer::SceneRendererSettings::CullMode::GPU;
                const bool bloomKawase = s.Bloom && s.Kernel == Renderer::BloomKernel::Kawase;
                const bool debugView = s.Mode != Renderer::DebugView::Final;
                // The sub-rect DRS scaling is applied only on this exact battery set; anything else
                // forces full resolution (ResolveRenderScale), so the effective extent is valid_extent
                // regardless of render_scale. The temporal (TAA/TAAU) resolve is sub-rect-aware and no
                // longer forces full resolution, but depth of field composited after it does.
                const bool subRectApplied = !s.SSR && !(cullGpu && s.Occlusion) && !bloomKawase &&
                                            !debugView && !(s.UsesTaa() && s.DepthOfField);
                Json result = {
                    {"visible", renderer.GetLastVisibleCount()},
                    {"frustum_survived", renderer.GetFrustumSurvivedCount()},
                    {"drawn", renderer.GetLastDrawnCount()},
                    {"gpu_survivors", renderer.GetLastGpuSurvivorCount()},
                    {"broadphase_rebuilt", renderer.DidBroadphaseRebuildLastFrame()},
                    {"broadphase_nodes", renderer.GetBroadphaseNodeCount()},
                    {"gpu_frame_time_ms", host.Assets.GetContext().GetLastGpuFrameTimeMs()},
                    {"render_scale", viewport->GetRenderScale()},
                    {"allocation_scale", viewport->GetAllocationScale()},
                    {"allocation_extent", {alloc.x, alloc.y}},
                    {"valid_extent", {valid.x, valid.y}},
                    {"sub_rect_applied", subRectApplied},
                    {"render_features",
                     {{"aa", Veng::string(Renderer::AntiAliasingModeNames[static_cast<Veng::usize>(
                                 s.AntiAliasing)])},
                      {"ssr", s.SSR},
                      {"cull_gpu", cullGpu},
                      {"occlusion", s.Occlusion},
                      {"bloom_kawase", bloomKawase},
                      {"debug_view", debugView}}},
                    {"dynamic_resolution", viewport->IsDynamicResolutionEnabled()}};
                if (const optional<Renderer::DynamicResolutionSettings>& drs =
                        viewport->GetDynamicResolution();
                    drs.has_value())
                {
                    result["drs_min_scale"] = drs->MinScale;
                    result["drs_max_scale"] = drs->MaxScale;
                    result["drs_target_frame_ms"] = drs->TargetFrameTimeMs;
                }
                return result.dump();
            };
            server.RegisterTool(std::move(tool));
        }

        // render.pass_times — the last frame's per-pass GPU timings, from the backend's timestamp
        // queries (independent of the profiler, so available in any build). This is the per-pass
        // breakdown of the single number render.stats reports as gpu_frame_time_ms.
        {
            McpTool tool;
            tool.Name = "render.pass_times";
            tool.Description =
                "Reports the last completed frame's per-pass GPU timings — each RenderGraph pass's "
                "name and GPU duration in milliseconds, in submission order, plus the whole-frame "
                "GPU time. Sourced from the backend's timestamp queries (not the profiler), so it "
                "is available in any build where the device supports GPU timestamps; an empty "
                "'passes' with 'gpu_timing_supported' false means the device does not. A pass name "
                "may repeat (per-mip bloom, per-face captures); the entries are the raw scopes. "
                "Takes no arguments — the timings are the render context's, not a viewport's.";
            tool.InputSchemaJson = R"({"type":"object","properties":{}})";
            tool.Handler = [&host](string_view /*argsJson*/) -> Result<string>
            {
                const Renderer::Context& context = host.Assets.GetContext();
                const std::span<const Renderer::Context::GpuPassTiming> timings =
                    context.GetLastGpuPassTimings();
                Json passes = Json::array();
                for (const Renderer::Context::GpuPassTiming& pass : timings)
                {
                    passes.push_back(Json{{"name", pass.Name},
                                          {"ms", pass.Milliseconds},
                                          {"begin_ns", pass.BeginNanos},
                                          {"end_ns", pass.EndNanos}});
                }
                return Json{{"gpu_frame_time_ms", context.GetLastGpuFrameTimeMs()},
                            {"gpu_timing_supported", !timings.empty()},
                            {"passes", std::move(passes)}}
                    .dump();
            };
            server.RegisterTool(std::move(tool));
        }

        // render.bindless — how much of each arrayed binding is left. Every one has a fixed
        // capacity whose exhaustion is a fatal assert on an otherwise ordinary registration, and
        // nothing warns on the way down — a free list just gets shorter. Read across a consumer's
        // own open/close cycle it separates the two ways an array runs out: a count that returns to
        // where it started names simultaneous occupancy, one that steps down per cycle names a leak.
        {
            McpTool tool;
            tool.Name = "render.bindless";
            tool.Description =
                "Reports the bindless registry's seven arrayed bindings: free slots and total "
                "capacity for textures, volumes, cubes, samplers, storage images, storage buffers, "
                "and materials. This is the 'how much is left' read; render.bindless_slots is the "
                "'what is in there' one. Takes no arguments.";
            tool.InputSchemaJson = R"({"type":"object","properties":{}})";
            tool.Handler = [&host](string_view) -> Result<string>
            {
                using Registry = Renderer::BindlessRegistry;
                const Renderer::BindlessCapacity free =
                    host.Assets.GetContext().GetBindlessRegistry().GetFreeSlots();
                return Json{
                    {"textures", {{"free", free.Textures}, {"capacity", Registry::MaxTextures}}},
                    {"volumes", {{"free", free.Volumes}, {"capacity", Registry::MaxVolumes}}},
                    {"cubes", {{"free", free.Cubes}, {"capacity", Registry::MaxCubes}}},
                    {"samplers", {{"free", free.Samplers}, {"capacity", Registry::MaxSamplers}}},
                    {"storage_images",
                     {{"free", free.StorageImages}, {"capacity", Registry::MaxStorageImages}}},
                    {"storage_buffers",
                     {{"free", free.StorageBuffers}, {"capacity", Registry::MaxStorageBuffers}}},
                    {"materials", {{"free", free.Materials}, {"capacity", Registry::MaxMaterials}}},
                }
                    .dump();
            };
            server.RegisterTool(std::move(tool));
        }

        // render.bindless_slots — what one array actually holds. A free count says a build is at
        // 80 % of MaxTextures; only the occupants say whether it is holding what it needs or the
        // same atlas nine times, which is the difference between a budget to raise and a leak to
        // fix.
        {
            McpTool tool;
            tool.Name = "render.bindless_slots";
            tool.Description =
                "Lists the slots of one bindless array with what each holds — the per-slot "
                "companion to render.bindless's free/capacity summary, and the way to find what is "
                "occupying an array that is running out. 'array' names it: textures, volumes, "
                "cubes, samplers, storage_images, storage_buffers, materials. An image array's "
                "slot reports the view's debug name, its format, the image extent, the mips and "
                "layers the view exposes, and the tightly-packed bytes those subresources occupy "
                "(the codec's own footprint, so a compressed texture reports compressed bytes); a "
                "storage buffer reports its name and size; a sampler its name; a material its "
                "cached parameter block's length. Optional 'state' filters by slot state — "
                "'occupied' (the default), 'free', 'pending_release' (released but still inside "
                "its deferred-release window, which is why a free count can trail the unoccupied "
                "count), or 'all'. Paginated on { limit, cursor }; 'capacity', 'free' and "
                "'matched' report the whole array, not the page.";
            tool.InputSchemaJson = R"({"type":"object","properties":{"array":{"type":"string"},)"
                                   R"("state":{"type":"string"},"limit":{"type":"integer"},)"
                                   R"("cursor":{"type":"string"}},"required":["array"]})";
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                const Json args = Json::parse(argsJson, nullptr, false);
                if (!args.is_object() || !args.contains("array") || !args["array"].is_string())
                {
                    return std::unexpected(string{
                        "'array' is required and names one of: textures, volumes, cubes, samplers, "
                        "storage_images, storage_buffers, materials"});
                }
                const string arrayName = args["array"].get<string>();
                const optional<Renderer::BindlessArray> array =
                    Renderer::ParseBindlessArray(arrayName);
                if (!array)
                {
                    return std::unexpected(fmt::format(
                        "'{}' names no bindless array; expected one of: textures, volumes, cubes, "
                        "samplers, storage_images, storage_buffers, materials",
                        arrayName));
                }
                const optional<SlotFilter> filter = ParseSlotFilter(args);
                if (!filter)
                {
                    return std::unexpected(
                        string{"'state' must be one of: occupied, free, pending_release, all"});
                }

                const Renderer::BindlessRegistry& registry =
                    host.Assets.GetContext().GetBindlessRegistry();
                const vector<Renderer::BindlessSlot> slots = registry.DescribeSlots(*array);
                const u32 cursor = ParseSlotCursor(args);
                const u32 limit = ParseSlotLimit(args);

                // 'matched' counts the whole array so a page reports how much of the filtered set
                // it is; the walk therefore counts every slot and only pages the emitted ones.
                Json items = Json::array();
                u32 matched = 0;
                u32 nextCursor = 0;
                bool more = false;
                for (const Renderer::BindlessSlot& slot : slots)
                {
                    if (!Admits(*filter, slot.State))
                    {
                        continue;
                    }
                    ++matched;
                    if (slot.Index < cursor)
                    {
                        continue;
                    }
                    if (items.size() >= limit)
                    {
                        if (!more)
                        {
                            more = true;
                            nextCursor = slot.Index;
                        }
                        continue;
                    }
                    items.push_back(DescribeSlot(*array, slot));
                }

                const Renderer::BindlessCapacity free = registry.GetFreeSlots();
                const u32 freeCount = [&]
                {
                    switch (*array)
                    {
                    case Renderer::BindlessArray::Textures:
                        return free.Textures;
                    case Renderer::BindlessArray::Volumes:
                        return free.Volumes;
                    case Renderer::BindlessArray::Cubes:
                        return free.Cubes;
                    case Renderer::BindlessArray::Samplers:
                        return free.Samplers;
                    case Renderer::BindlessArray::StorageImages:
                        return free.StorageImages;
                    case Renderer::BindlessArray::StorageBuffers:
                        return free.StorageBuffers;
                    case Renderer::BindlessArray::Materials:
                        return free.Materials;
                    }
                    return 0u;
                }();

                Json out{{"array", Renderer::BindlessArrayName(*array)},
                         {"capacity", Renderer::BindlessRegistry::CapacityOf(*array)},
                         {"free", freeCount},
                         {"matched", matched},
                         {"slots", std::move(items)}};
                if (more)
                {
                    out["nextCursor"] = std::to_string(nextCursor);
                }
                return out.dump();
            };
            server.RegisterTool(std::move(tool));
        }
    }

    void RegisterRenderCaptureWriteTools(McpServer& server, const McpHost& host)
    {
        // render.capture_start — begin recording the presented frame. Every setting is optional
        // and defaults to the recorder's own; there is no directory argument, so a capture is
        // named and the engine places it.
        {
            McpTool tool;
            tool.Name = "render.capture_start";
            tool.Description =
                "Begins recording the presented frame to a video file through the platform's "
                "hardware encoder. Every setting is optional: 'encoding' (Auto/Sdr/Hdr10, Auto "
                "following the display), 'codec' (Hevc/H264/ProRes422HQ/ProRes4444), "
                "'bits_per_pixel' (quality; the bitrate is extent x rate x this), "
                "'bitrate_mbps' (an outright override; 0 derives), 'lockstep' (drive the frame "
                "clock so the file plays back at the full rate however slowly the machine "
                "renders, the audio sample-locked and silent meanwhile), 'frame_rate', "
                "'frame_budget' (stop after this many frames; 0 records until stopped), 'audio' "
                "(None/Pcm/Aac), 'include_overlay' (whether the application's own interface is "
                "composited in; off by default), and 'name'. There is no directory argument — "
                "the engine resolves the name under the capture directory. Returns the capture "
                "state; a refusal is a tool error carrying the reason.";
            tool.InputSchemaJson =
                R"({"type":"object","properties":{)"
                R"("encoding":{"type":"string","enum":["Auto","Sdr","Hdr10"]},)"
                R"("codec":{"type":"string","enum":["Hevc","H264","ProRes422HQ","ProRes4444"]},)"
                R"("bits_per_pixel":{"type":"number","exclusiveMinimum":0},)"
                R"("bitrate_mbps":{"type":"integer","minimum":0},)"
                R"("lockstep":{"type":"boolean"},)"
                R"("frame_rate":{"type":"integer","minimum":1},)"
                R"("frame_budget":{"type":"integer","minimum":0},)"
                R"("audio":{"type":"string","enum":["None","Pcm","Aac"]},)"
                R"("include_overlay":{"type":"boolean"},)"
                R"("name":{"type":"string"}},"additionalProperties":false})";
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                const Json args = Json::parse(argsJson, nullptr, false);
                const Result<Capture::VideoCaptureSettings> settings = ParseCaptureSettings(args);
                if (!settings)
                {
                    return std::unexpected(settings.error());
                }

                Capture::VideoRecorder* const recorder = ResolveRecorder(host);
                if (recorder == nullptr)
                {
                    return std::unexpected(string(RecorderUnavailable));
                }
                if (!recorder->Start(settings.value()))
                {
                    const Capture::VideoCaptureState state = recorder->GetState();
                    return std::unexpected(state.LastError.empty()
                                               ? string("the capture was refused")
                                               : state.LastError);
                }
                return CaptureStateJson(recorder->GetState(), recorder->IsAvailable()).dump();
            };
            server.RegisterTool(std::move(tool));
        }

        // render.capture_stop — request the stop and report the state it left. Finalization is
        // asynchronous, so the file is complete once render.capture_status reports Off.
        {
            McpTool tool;
            tool.Name = "render.capture_stop";
            tool.Description =
                "Ends the running video capture and returns the recorder's state as the stop "
                "leaves it. The file is committed asynchronously, so the state may read "
                "Finalizing; poll render.capture_status until 'status' is Off for a file that is "
                "finished on disk. A no-op when nothing is recording.";
            tool.InputSchemaJson = R"({"type":"object","properties":{}})";
            tool.Handler = [&host](string_view) -> Result<string>
            {
                Capture::VideoRecorder* const recorder = ResolveRecorder(host);
                if (recorder == nullptr)
                {
                    return std::unexpected(string(RecorderUnavailable));
                }
                recorder->Stop();
                return CaptureStateJson(recorder->GetState(), recorder->IsAvailable()).dump();
            };
            server.RegisterTool(std::move(tool));
        }
    }
}
