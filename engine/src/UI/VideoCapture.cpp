#include <Veng/UI/VideoCapture.h>

#include "../Capture/RecorderCore.h"

#include <Veng/Diagnostics/Profiler.h>
#include <Veng/UI/Layout.h>
#include <Veng/UI/Query.h>
#include <Veng/UI/Scopes.h>
#include <Veng/UI/Theme.h>
#include <Veng/UI/Widgets.h>

#include <fmt/format.h>

#include <array>
#include <span>

namespace Veng::UI
{
    using Capture::CaptureEncoding;
    using Capture::VideoCaptureSettings;
    using Capture::VideoCaptureState;
    using Capture::VideoCaptureStatus;
    using Capture::VideoCodec;

    namespace
    {
        /// @brief The encoding arms, in enumerator order, as the combo draws them.
        constexpr std::array<string_view, 3> EncodingNames{"Auto (follow the display)", "SDR",
                                                           "HDR10"};

        /// @brief The sound-track arms, in enumerator order, as the combo draws them.
        constexpr std::array<string_view, 3> AudioNames{"None", "PCM (uncompressed)", "AAC"};

        /// @brief The codec arms in enumerator order, for the combo's rows.
        constexpr std::array<VideoCodec, 4> Codecs{VideoCodec::Hevc, VideoCodec::H264,
                                                   VideoCodec::ProRes422HQ, VideoCodec::ProRes4444};

        /// @brief Returns @p text with surrounding whitespace removed.
        /// @param text  The text to trim.
        /// @return The trimmed view into @p text.
        string_view Trim(string_view text)
        {
            constexpr string_view Space = " \t\n\r\f\v";
            const usize first = text.find_first_not_of(Space);
            if (first == string_view::npos)
            {
                return {};
            }
            return text.substr(first, text.find_last_not_of(Space) - first + 1);
        }

        /// @brief Draws a combo over @p names selecting one arm of an enum in enumerator order.
        /// @param label  Widget label and ImGui id.
        /// @param value  The enum edited in place.
        /// @param names  The arm labels, in enumerator order.
        /// @return True the frame the selection changed.
        template <typename Enum>
        bool EnumCombo(string_view label, Enum& value, std::span<const string_view> names)
        {
            i32 index = static_cast<i32>(value);
            if (!UI::Combo(label, index, names))
            {
                return false;
            }
            value = static_cast<Enum>(index);
            return true;
        }

        /// @brief Draws the codec combo, greying an arm that cannot carry the chosen encoding.
        ///
        /// The reason rides the greyed row's own label rather than a tooltip: a disabled item does
        /// not register as hovered, so a tooltip on one would never appear.
        /// @param settings  The settings whose codec is edited in place.
        void CodecCombo(VideoCaptureSettings& settings)
        {
            const string preview = Capture::DescribeCodec(settings.Codec, settings.Encoding);
            if (auto combo = UI::ComboBox("Codec", preview))
            {
                for (const VideoCodec codec : Codecs)
                {
                    const bool supported = Capture::CodecSupportsEncoding(codec, settings.Encoding);
                    auto disabled = UI::Disabled(!supported);
                    const string label =
                        supported
                            ? Capture::DescribeCodec(codec, settings.Encoding)
                            : fmt::format("{} — no ten-bit profile for HDR10",
                                          Capture::DescribeCodec(codec, CaptureEncoding::Sdr));
                    if (UI::Selectable(label, codec == settings.Codec) && supported)
                    {
                        settings.Codec = codec;
                    }
                }
            }
        }

        /// @brief Draws the quality controls: bits per pixel, the megabit figure, and the override.
        /// @param settings  The settings edited in place.
        /// @param extent    The extent the megabit figure is derived at; zero when none is known.
        void QualityRows(VideoCaptureSettings& settings, uvec2 extent)
        {
            (void)UI::Drag("Quality (bits/pixel)", settings.BitsPerPixel,
                           DragOptions{.Speed = 0.005f, .Min = 0.01f, .Max = 2.0f});

            if (settings.BitrateMbps != 0)
            {
                UI::TextDisabled(fmt::format("{} Mb/s (override)", settings.BitrateMbps));
            }
            else if (extent.x != 0 && extent.y != 0)
            {
                UI::TextDisabled(fmt::format("{} Mb/s at {}x{}",
                                             UI::CaptureBitrateMbps(settings, extent), extent.x,
                                             extent.y));
            }
            else
            {
                UI::TextDisabled("derived from the frame size when the capture starts");
            }

            i32 bitrate = static_cast<i32>(settings.BitrateMbps);
            if (UI::Drag("Bitrate override (Mb/s, 0 derives)", bitrate,
                         DragOptions{.Speed = 0.5f, .Min = 0.0f, .Max = 2000.0f}))
            {
                settings.BitrateMbps = static_cast<u32>(bitrate);
            }
        }

        /// @brief Draws the clock, budget, audio, overlay and name rows.
        /// @param state  The panel state edited in place.
        void CaptureRows(VideoCapturePanelState& state)
        {
            VideoCaptureSettings& settings = state.Settings;

            (void)UI::Checkbox("Lockstep", settings.Lockstep);
            UI::Tooltip("Every frame is simulated, rendered and encoded before the next begins, so "
                        "the file plays back at the full rate however slowly the machine renders. "
                        "Nothing is heard for the capture's duration.");

            {
                auto disabled = UI::Disabled(!settings.Lockstep);
                i32 rate = static_cast<i32>(settings.FrameRate);
                if (UI::Drag("Frame rate", rate,
                             DragOptions{.Speed = 0.25f, .Min = 1.0f, .Max = 240.0f}))
                {
                    settings.FrameRate = static_cast<u32>(rate);
                }
            }

            i32 budget = static_cast<i32>(settings.FrameBudget);
            if (UI::Drag("Frame budget (0 = until stopped)", budget,
                         DragOptions{.Speed = 1.0f, .Min = 0.0f, .Max = 1000000.0f}))
            {
                settings.FrameBudget = static_cast<u64>(budget);
            }

            (void)EnumCombo("Audio", settings.Audio, AudioNames);

            (void)UI::Checkbox("Include overlay", settings.IncludeOverlay);
            UI::Tooltip("The overlay is the application's own interface — this panel among it. A "
                        "world's HUD and menus are drawn into the viewport and are recorded either "
                        "way.");

            (void)UI::InputTextWithHint("Name", "<app>-<yyyymmdd-hhmmss>", state.Name);
        }

        /// @brief Draws the running capture's figures, and the two warning rows when they are non-zero.
        /// @param live  The recorder's state this frame.
        void LiveRows(const VideoCaptureState& live)
        {
            UI::Label("Codec", live.Codec);
            UI::Label("Encoding",
                      live.Encoding == CaptureEncoding::Hdr10 ? string("HDR10") : string("SDR"));
            UI::Label("Extent", fmt::format("{}x{}", live.Extent.x, live.Extent.y));
            UI::Label("Frames", live.FrameBudget != 0
                                    ? fmt::format("{} / {} ({} appended)", live.FramesAcquired,
                                                  live.FrameBudget, live.FramesAppended)
                                    : fmt::format("{} ({} appended)", live.FramesAcquired,
                                                  live.FramesAppended));
            UI::Label("Duration", fmt::format("{:.2f} s", live.DurationSeconds));
            UI::Label("File size", fmt::format("{:.1f} MiB", static_cast<f64>(live.BytesWritten) /
                                                                 (1024.0 * 1024.0)));
            UI::Label("Audio blocks", fmt::format("{}", live.AudioBlocks));

            const Theme& theme = UI::GetTheme();
            if (live.FramesWaited != 0)
            {
                UI::TextColored(theme.Warning,
                                fmt::format("encoder behind: {} frames waited, {:.0f} ms total",
                                            live.FramesWaited, live.WaitedForEncoderMs));
            }
            if (live.AudioOverruns != 0)
            {
                UI::TextColored(
                    theme.Warning,
                    fmt::format("{} audio blocks lost, written as silence", live.AudioOverruns));
            }
        }
    }

    VideoCaptureSettings ResolveCaptureSettings(const VideoCapturePanelState& state)
    {
        VideoCaptureSettings settings = state.Settings;
        settings.Name = string(Trim(state.Name));
        return settings;
    }

    u32 CaptureBitrateMbps(const VideoCaptureSettings& settings, uvec2 extent)
    {
        constexpr u64 BitsPerMegabit = 1000000;
        const u64 bits = Capture::DeriveBitsPerSecond(settings, extent);
        return static_cast<u32>((bits + BitsPerMegabit / 2) / BitsPerMegabit);
    }

    void VideoCapturePanel(Capture::VideoRecorder& recorder, VideoCapturePanelState& state)
    {
        // Reading the state is also what advances a finished capture from Finalizing to Off, so the
        // one read per frame is the panel's whole share of the recorder's bookkeeping.
        const VideoCaptureState live = recorder.GetState();
        const bool recording = live.Status == VideoCaptureStatus::Recording;
        const bool finalizing = live.Status == VideoCaptureStatus::Finalizing;

        {
            auto disabled = UI::Disabled(recording || finalizing);
            (void)EnumCombo("Encoding", state.Settings.Encoding, EncodingNames);
            CodecCombo(state.Settings);
            QualityRows(state.Settings, live.Extent);
            CaptureRows(state);
        }
        UI::TextDisabled(fmt::format("Output: {}", Diagnostics::CaptureDirectory().string()));

        UI::Separator();

        if (recording || finalizing)
        {
            LiveRows(live);
        }

        if (finalizing)
        {
            auto disabled = UI::Disabled();
            UI::Text("Writing file…");
        }
        else if (recording)
        {
            if (UI::Button("Stop"))
            {
                recorder.Stop();
            }
            UI::TextDisabled("Closing this window does not stop the capture, and the window is not "
                             "in the recording unless Include overlay was set.");
        }
        else if (!recorder.IsAvailable())
        {
            UI::TextDisabled("Video capture is unavailable: this run has no encoder behind a "
                             "shareable surface, or presents no frame to record.");
        }
        else if (UI::Button("Start"))
        {
            (void)recorder.Start(ResolveCaptureSettings(state));
        }

        if (!live.Path.empty())
        {
            UI::TextDisabled(fmt::format("File: {}", live.Path.string()));
        }
        if (!live.LastError.empty())
        {
            UI::TextColored(UI::GetTheme().Warning, live.LastError);
        }
    }
}
