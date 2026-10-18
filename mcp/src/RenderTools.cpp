#include "RenderTools.h"

#include "ViewportCapture.h"

#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpTool.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/HexId.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Capture/FrameCapture.h>
#include <Veng/Capture/VideoRecorder.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/FormatInfo.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/TextureReadback.h>
#include <Veng/Renderer/Viewport.h>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
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

        /// @brief Every key render.configure accepts; anything else is rejected as unknown.
        constexpr std::array<string_view, 4> ConfigureKeys{
            "viewport", "debug_view", "light_tile_culling", "gbuffer_shading_override"};

        /// @brief The renderer debug settings render.configure reports, by their argument names.
        Json DebugSettingsJson(const Renderer::SceneRendererSettings& settings)
        {
            return Json{
                {"debug_view", string(Renderer::DebugViewNames[static_cast<usize>(settings.Mode)])},
                {"light_tile_culling", settings.LightTileCulling},
                {"gbuffer_shading_override", settings.GBufferShadingOverride}};
        }

        /// @brief Applies a render.configure argument object onto a viewport's settings.
        ///
        /// Every key is optional and an absent one leaves its setting as it is. An unknown key, a
        /// debug view naming no DebugView arm, and a toggle that is not a boolean are each a tool
        /// error, and a refused call applies nothing.
        /// @param args      The tools/call arguments object.
        /// @param settings  The viewport's current settings, updated in place.
        /// @return Nothing, or the reason the arguments were refused.
        VoidResult ApplyDebugSettings(const Json& args, Renderer::SceneRendererSettings& settings)
        {
            if (!args.is_object())
            {
                return {};
            }
            for (const auto& [key, value] : args.items())
            {
                if (std::ranges::find(ConfigureKeys, key) == ConfigureKeys.end())
                {
                    return std::unexpected(
                        fmt::format("unknown setting '{}'; one of debug_view, light_tile_culling, "
                                    "gbuffer_shading_override",
                                    key));
                }
            }

            Renderer::SceneRendererSettings next = settings;
            if (args.contains("debug_view"))
            {
                const Json& value = args["debug_view"];
                const optional<usize> index =
                    value.is_string()
                        ? FindEnumerator(value.get<string>(), Renderer::DebugViewNames)
                        : std::nullopt;
                if (!index)
                {
                    return std::unexpected(fmt::format("'debug_view' names a DebugView arm ({})",
                                                       fmt::join(Renderer::DebugViewNames, ", ")));
                }
                next.Mode = static_cast<Renderer::DebugView>(*index);
            }
            const auto boolArg = [&args](const string& key, bool& field) -> VoidResult
            {
                if (!args.contains(key))
                {
                    return {};
                }
                if (!args[key].is_boolean())
                {
                    return std::unexpected(fmt::format("'{}' is a boolean", key));
                }
                field = args[key].get<bool>();
                return {};
            };
            if (VoidResult applied = boolArg("light_tile_culling", next.LightTileCulling); !applied)
            {
                return applied;
            }
            if (VoidResult applied =
                    boolArg("gbuffer_shading_override", next.GBufferShadingOverride);
                !applied)
            {
                return applied;
            }
            settings = next;
            return {};
        }

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
                        "resolves 'name' under the user's recordings directory",
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

        /// @brief An image render.export_texture resolved, and the words its reply names it by.
        struct ExportSource
        {
            /// @brief The image to read back.
            Ref<Renderer::Image> Image;
            /// @brief How the reply describes where it came from.
            string Label;
        };

        /// @brief Resolves render.export_texture's source: exactly one of sky, asset, or array+slot.
        Result<ExportSource> ResolveExportSource(const McpHost& host, const Json& args)
        {
            const bool sky =
                args.contains("sky") && args["sky"].is_boolean() && args["sky"].get<bool>();
            const bool asset = args.contains("asset");
            const bool slot = args.contains("array") || args.contains("slot");
            if (static_cast<int>(sky) + static_cast<int>(asset) + static_cast<int>(slot) != 1)
            {
                return std::unexpected(string(
                    "name exactly one source: 'sky': true, 'asset' (a texture's hex AssetId), or "
                    "'array' + 'slot' (as render.bindless_slots reports them)"));
            }

            if (sky)
            {
                const string name = ViewportName(args);
                Renderer::Viewport* viewport = ResolveViewport(host, name);
                if (viewport == nullptr)
                {
                    return std::unexpected(name.empty()
                                               ? string("no primary viewport is available")
                                               : fmt::format("no viewport named '{}'", name));
                }
                Ref<Renderer::Image> cube = viewport->GetRenderer().GetDisplayedSkyCube();
                if (!cube)
                {
                    return std::unexpected(string(
                        "the viewport's sky is not a cube (no sky, or one drawn per pixel)"));
                }
                return ExportSource{.Image = std::move(cube), .Label = "sky radiance cube"};
            }

            if (asset)
            {
                const optional<AssetId> id = args["asset"].is_string()
                                                 ? ParseAssetId(args["asset"].get<string>())
                                                 : std::nullopt;
                if (!id)
                {
                    return std::unexpected(string("'asset' is a hex AssetId string"));
                }
                const AssetResult<AssetHandle<Texture>> texture =
                    host.Assets.LoadSync<Texture>(*id);
                if (!texture.has_value())
                {
                    return std::unexpected(fmt::format("texture {} did not load: {}",
                                                       FormatAssetId(*id), texture.error().Detail));
                }
                return ExportSource{.Image = texture.value().Get()->GetImage(),
                                    .Label = fmt::format("texture {}", FormatAssetId(*id))};
            }

            const optional<Renderer::BindlessArray> array =
                args.contains("array") && args["array"].is_string()
                    ? Renderer::ParseBindlessArray(args["array"].get<string>())
                    : std::nullopt;
            if (!array || !args.contains("slot") || !args["slot"].is_number_integer())
            {
                return std::unexpected(string(
                    "'array' names an image array (textures, volumes, cubes, storage_images) and "
                    "'slot' is its integer index"));
            }
            const u32 index = args["slot"].get<u32>();
            const Ref<Renderer::ImageView> view =
                host.Assets.GetContext().GetBindlessRegistry().GetSlotView(*array, index);
            if (!view || !view->GetImage())
            {
                return std::unexpected(fmt::format("{} slot {} holds no image",
                                                   Renderer::BindlessArrayName(*array), index));
            }
            return ExportSource{.Image = view->GetImage(),
                                .Label = fmt::format("{} slot {} ({})",
                                                     Renderer::BindlessArrayName(*array), index,
                                                     view->GetName())};
        }

        /// @brief Rec. 709 luminance statistics over a readback, for the export's text block.
        ///
        /// A six-layer square readback is read as a cube, and its mean is also reported weighted by
        /// each texel's solid angle — the mean radiance an observer at the centre sees, which the
        /// unweighted mean overstates toward the face corners.
        Json LuminanceStats(const Renderer::TexelReadback& readback)
        {
            vector<f32> luma;
            luma.reserve(readback.Texels.size());
            f64 sum = 0.0;
            usize nonFinite = 0;
            for (const vec4& texel : readback.Texels)
            {
                const f32 value = 0.2126f * texel.r + 0.7152f * texel.g + 0.0722f * texel.b;
                if (!std::isfinite(value))
                {
                    ++nonFinite;
                    continue;
                }
                luma.push_back(value);
                sum += value;
            }
            Json stats{{"texels", readback.Texels.size()}, {"non_finite", nonFinite}};
            if (luma.empty())
            {
                return stats;
            }
            std::ranges::sort(luma);
            const auto at = [&luma](const f64 fraction)
            {
                const usize index = std::min(
                    luma.size() - 1, static_cast<usize>(fraction * static_cast<f64>(luma.size())));
                return luma[index];
            };
            stats["min"] = luma.front();
            stats["max"] = luma.back();
            stats["mean"] = sum / static_cast<f64>(luma.size());
            stats["percentiles"] = Json{
                {"p01", at(0.01)}, {"p10", at(0.10)},   {"p50", at(0.50)},    {"p90", at(0.90)},
                {"p99", at(0.99)}, {"p999", at(0.999)}, {"p9999", at(0.9999)}};

            if (readback.Layers == 6 && readback.Extent.x == readback.Extent.y)
            {
                const u32 size = readback.Extent.x;
                f64 weighted = 0.0;
                f64 weights = 0.0;
                for (u32 layer = 0; layer < 6; ++layer)
                {
                    for (u32 y = 0; y < size; ++y)
                    {
                        const f64 v = (2.0 * (y + 0.5) / size) - 1.0;
                        for (u32 x = 0; x < size; ++x)
                        {
                            const f64 u = (2.0 * (x + 0.5) / size) - 1.0;
                            const vec4 texel =
                                readback.Texels[(static_cast<usize>(layer) * size + y) * size + x];
                            const f64 value =
                                0.2126 * texel.r + 0.7152 * texel.g + 0.0722 * texel.b;
                            if (!std::isfinite(value))
                            {
                                continue;
                            }
                            const f64 weight = 1.0 / std::pow(1.0 + u * u + v * v, 1.5);
                            weighted += value * weight;
                            weights += weight;
                        }
                    }
                }
                stats["mean_solid_angle_weighted"] = weighted / weights;
            }
            return stats;
        }
    }

    void RegisterRenderTools(McpServer& server, const McpHost& host)
    {
        // render.export_texture — one mip of an image (the sky cube a viewport shows, a texture
        // asset, or a bindless slot) read back as float and written as a file.
        {
            McpTool tool;
            tool.Name = "render.export_texture";
            tool.Description =
                "Exports one mip of a texture or cubemap as an image file. Name exactly one "
                "source: "
                "'sky': true (the radiance cube the viewport's skybox shows; optional 'viewport'), "
                "'asset' (a Texture's hex AssetId), or 'array' + 'slot' (a bindless image slot, as "
                "render.bindless_slots lists them). Optional 'mip' (default 0) and 'encoding': "
                "'hdr' (Radiance RGBE, linear float — the default for float formats) or 'png' "
                "(8-bit, the default otherwise). Layers sit side by side, a cube's in the order "
                "+X -X +Y -Y +Z -Z. The text block reports the extent, format and luminance "
                "statistics. Over the --connect CLI it requires --output <file>.";
            tool.InputSchemaJson =
                R"({"type":"object","properties":{"sky":{"type":"boolean"},"viewport":{"type":"string"},"asset":{"type":"string"},"array":{"type":"string"},"slot":{"type":"integer"},"mip":{"type":"integer"},"encoding":{"type":"string","enum":["hdr","png"]}}})";
            tool.ReturnsContentBlocks = true;
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                const Json parsed = Json::parse(argsJson, nullptr, false);
                const Json args = parsed.is_object() ? parsed : Json::object();
                const Result<ExportSource> source = ResolveExportSource(host, args);
                if (!source)
                {
                    return std::unexpected(source.error());
                }

                const u32 mip = args.contains("mip") && args["mip"].is_number_integer()
                                    ? args["mip"].get<u32>()
                                    : 0;
                const Result<Renderer::TexelReadback> readback = Renderer::ReadbackTexels(
                    host.Assets.GetContext(), host.Assets, source->Image, mip);
                if (!readback)
                {
                    return std::unexpected(readback.error());
                }

                const Renderer::Format format = readback->SourceFormat;
                const bool floatFormat = format == Renderer::Format::R16Sfloat ||
                                         format == Renderer::Format::RG16Sfloat ||
                                         format == Renderer::Format::RGBA16Sfloat ||
                                         format == Renderer::Format::R32Sfloat ||
                                         format == Renderer::Format::RG32Sfloat ||
                                         format == Renderer::Format::RGB32Sfloat ||
                                         format == Renderer::Format::RGBA32Sfloat ||
                                         format == Renderer::Format::B10G11R11Ufloat;
                TexelEncoding encoding =
                    floatFormat ? TexelEncoding::RadianceHdr : TexelEncoding::Png;
                if (args.contains("encoding"))
                {
                    const string requested =
                        args["encoding"].is_string() ? args["encoding"].get<string>() : string{};
                    if (requested == "hdr")
                    {
                        encoding = TexelEncoding::RadianceHdr;
                    }
                    else if (requested == "png")
                    {
                        encoding = TexelEncoding::Png;
                    }
                    else
                    {
                        return std::unexpected(string("'encoding' is 'hdr' or 'png'"));
                    }
                }

                const Json text{
                    {"source", source->Label},
                    {"format", Renderer::FormatName(format)},
                    {"extent", Json::array({readback->Extent.x, readback->Extent.y})},
                    {"layers", readback->Layers},
                    {"mip", mip},
                    {"encoding", encoding == TexelEncoding::RadianceHdr ? "hdr" : "png"},
                    {"layout", readback->Layers == 6 ? "faces left to right: +X -X +Y -Y +Z -Z"
                                                     : "layers left to right"},
                    {"luminance", LuminanceStats(*readback)}};
                return TexelContentBlocks(*readback, encoding, text.dump());
            };
            server.RegisterTool(std::move(tool));
        }

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

        // render.screenshot_window — the composited frame, the only picture carrying the app's own
        // overlay, or the frame with that overlay left out.
        {
            McpTool tool;
            tool.Name = "render.screenshot_window";
            tool.Description =
                "Captures the composited frame as a PNG image — the presented viewports with the "
                "app's own UI overlay drawn over them, which is what an app's interface looks like "
                "on screen. Optional 'overlay' (default true): false leaves the app's own overlay "
                "(a debug shell, an editor's panels) out, keeping any GUI a world drives into its "
                "viewports. render.screenshot captures one viewport instead. The picture is sRGB "
                "whatever the display. Returns an image content block; over the --connect CLI it "
                "requires --output <file> to write the PNG (an image is never printed to stdout). "
                "Unavailable headless, where no frame is composited.";
            tool.InputSchemaJson =
                R"({"type":"object","properties":{"overlay":{"type":"boolean"}}})";
            tool.ReturnsContentBlocks = true;
            // The frame is composited into the capture's image by the frame that follows the
            // request, so the call asks at its first pump and takes the picture at the next.
            tool.BeforeFrame = [&host](string_view argsJson) -> VoidResult
            {
                Capture::FrameCapture* const capture =
                    host.FrameCapture ? host.FrameCapture() : nullptr;
                if (capture == nullptr)
                {
                    return std::unexpected(
                        string("frame capture is unavailable: this host exposes none"));
                }

                const Json args = Json::parse(argsJson, nullptr, false);
                bool includeOverlay = true;
                if (args.is_object() && args.contains("overlay"))
                {
                    if (!args["overlay"].is_boolean())
                    {
                        return std::unexpected(string("'overlay' must be a boolean"));
                    }
                    includeOverlay = args["overlay"].get<bool>();
                }
                return capture->Request({.IncludeOverlay = includeOverlay});
            };
            tool.Handler = [&host](string_view) -> Result<string>
            {
                Capture::FrameCapture* const capture =
                    host.FrameCapture ? host.FrameCapture() : nullptr;
                if (capture == nullptr)
                {
                    return std::unexpected(
                        string("frame capture is unavailable: this host exposes none"));
                }
                return TakeFrameContentBlocks(*capture);
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
                const bool debugView = s.Mode != Renderer::DebugView::Final;
                // The sub-rect DRS scaling is applied only on this exact battery set; anything else
                // forces full resolution (ResolveRenderScale), so the effective extent is valid_extent
                // regardless of render_scale. The temporal (TAA/TAAU) resolve is sub-rect-aware and no
                // longer forces full resolution, but depth of field composited after it does.
                const bool subRectApplied =
                    !(cullGpu && s.Occlusion) && !debugView && !(s.UsesTaa() && s.DepthOfField);
                Json result = {
                    {"visible", renderer.GetLastVisibleCount()},
                    {"frustum_survived", renderer.GetFrustumSurvivedCount()},
                    {"drawn", renderer.GetLastDrawnCount()},
                    {"gpu_survivors", renderer.GetLastGpuSurvivorCount()},
                    {"broadphase_rebuilt", renderer.DidBroadphaseRebuildLastFrame()},
                    {"broadphase_refit", renderer.DidBroadphaseRefitLastFrame()},
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
                      {"upscale",
                       Veng::string(
                           Renderer::UpscaleFilterNames[static_cast<Veng::usize>(s.Upscale)])},
                      {"ssr", s.SSR},
                      {"cull_gpu", cullGpu},
                      {"occlusion", s.Occlusion},
                      {"debug_view", debugView},
                      {"light_tile_culling", s.LightTileCulling},
                      {"gbuffer_shading_override", s.GBufferShadingOverride}}},
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
                "capacity for textures, volumes, cubes, samplers, storage images and storage "
                "buffers. Materials are suballocated from a byte arena rather than drawn from a "
                "slot table, so they report free and total BYTES plus the live block count and "
                "the largest free run — free bytes far above the largest run is fragmentation. "
                "This is the 'how much is left' read; render.bindless_slots is the 'what is in "
                "there' one. Takes no arguments.";
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
                    {"materials",
                     {{"free_bytes", free.Materials},
                      {"capacity_bytes", Registry::MaterialArenaBytes},
                      {"live_blocks", free.MaterialBlocks},
                      {"largest_free_run_bytes", free.MaterialLargestFreeRun}}},
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
                "storage buffer reports its name and size; a sampler its name. Materials are not "
                "a slot array: they list one entry per RANGE of the parameter arena — a live "
                "block, a free run, or a run still inside its release window — with 'index' the "
                "range's byte offset and 'bytes' its length, so the listing reads as a map of the "
                "arena. Optional 'state' filters by slot state — "
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

    void RegisterRenderSettingsWriteTools(McpServer& server, const McpHost& host)
    {
        // render.configure — the renderer's diagnostic settings, so a capture can be scripted
        // around them: set, capture, set back.
        McpTool tool;
        tool.Name = "render.configure";
        tool.Description =
            "Sets a viewport's renderer debug settings and returns them as applied. Every "
            "argument is optional and an absent one is left as it is: 'debug_view' (a DebugView "
            "arm by name, e.g. Final, Albedo, Normal), 'light_tile_culling' (whether the lighting "
            "pass culls lights per screen tile), 'gbuffer_shading_override' (whether every opaque "
            "g-buffer draw shades through one trivial fragment stage: a constant albedo and the "
            "vertex normal over the same geometry and draws, so the 'Scene GBuffer' pass time in "
            "render.pass_times with it on is the pass without material shading). A change "
            "recompiles the viewport's render graph. Optional 'viewport' names the viewport "
            "(default the primary). A host's own interface may re-apply its settings over these.";
        tool.InputSchemaJson =
            R"({"type":"object","properties":{)"
            R"("viewport":{"type":"string"},)"
            R"("debug_view":{"type":"string"},)"
            R"("light_tile_culling":{"type":"boolean"},)"
            R"("gbuffer_shading_override":{"type":"boolean"}},"additionalProperties":false})";
        tool.Handler = [&host](string_view argsJson) -> Result<string>
        {
            const Json args = Json::parse(argsJson, nullptr, false);
            const string name = ViewportName(args);
            Renderer::Viewport* viewport = ResolveViewport(host, name);
            if (viewport == nullptr)
            {
                return std::unexpected(name.empty() ? string("no primary viewport is available")
                                                    : fmt::format("no viewport named '{}'", name));
            }
            Renderer::SceneRendererSettings settings = viewport->GetSettings();
            if (const VoidResult applied = ApplyDebugSettings(args, settings); !applied)
            {
                return std::unexpected(applied.error());
            }
            viewport->Configure(settings);
            return DebugSettingsJson(viewport->GetEffectiveSettings()).dump();
        };
        server.RegisterTool(std::move(tool));
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
                "the engine resolves the name under the user's recordings directory. Returns the "
                "capture state; a refusal is a tool error carrying the reason.";
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
