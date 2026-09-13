#include "RecorderCore.h"

#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Log.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <thread>

namespace Veng::Capture
{
    namespace
    {
        /// @brief The extension every capture carries: LPCM and ProRes are QuickTime-only.
        constexpr string_view CaptureExtension = ".mov";

        /// @brief Sleeps the default wait step: short enough not to add latency, long enough to yield.
        void DefaultWaitStep()
        {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }

        /// @brief Returns the wall-clock seconds the default clock reads.
        f64 DefaultNow()
        {
            const auto since = std::chrono::steady_clock::now().time_since_epoch();
            return std::chrono::duration<f64>(since).count();
        }
    }

    VoidResult ValidateCaptureSettings(const VideoCaptureSettings& settings, const u32 channels)
    {
        if (!CodecSupportsEncoding(settings.Codec, settings.Encoding))
        {
            return std::unexpected(
                fmt::format("{} cannot encode HDR10: its hardware encoder has no ten-bit profile.",
                            DescribeCodec(settings.Codec, CaptureEncoding::Sdr)));
        }

        if (settings.FrameRate == 0)
        {
            return std::unexpected("A capture's frame rate must be greater than zero.");
        }

        if (settings.BitrateMbps == 0 && settings.BitsPerPixel <= 0.0f)
        {
            return std::unexpected(
                "A capture's quality must be a positive number of bits per pixel, "
                "or a bitrate must be given outright.");
        }

        if (settings.Audio != AudioTrack::None && (channels < 1 || channels > 2))
        {
            return std::unexpected(fmt::format(
                "A capture's sound track carries one or two channels; the device mixes {}.",
                channels));
        }

        return {};
    }

    u64 DeriveBitsPerSecond(const VideoCaptureSettings& settings, const uvec2 extent)
    {
        if (settings.BitrateMbps != 0)
        {
            return static_cast<u64>(settings.BitrateMbps) * 1000000ULL;
        }

        const f64 bits = static_cast<f64>(extent.x) * static_cast<f64>(extent.y) *
                         static_cast<f64>(settings.FrameRate) *
                         static_cast<f64>(settings.BitsPerPixel);
        return bits <= 0.0 ? 0ULL : static_cast<u64>(std::llround(bits));
    }

    string DefaultCaptureName(const string_view appName, const std::time_t when)
    {
        string stem;
        stem.reserve(appName.size());
        for (const char c : appName)
        {
            const bool plain = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                               (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
            // A name reaches the file system, so anything that is not plainly a file-name character
            // becomes a hyphen rather than being dropped, which would run two words together.
            stem.push_back(plain ? c : '-');
        }
        if (stem.empty())
        {
            stem = "veng";
        }

        std::tm parts{};
#if defined(_WIN32)
        localtime_s(&parts, &when);
#else
        localtime_r(&when, &parts);
#endif
        return fmt::format("{}-{:04}{:02}{:02}-{:02}{:02}{:02}", stem, parts.tm_year + 1900,
                           parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min,
                           parts.tm_sec);
    }

    path ResolveVideoCapturePath(const string_view name)
    {
        path leaf = path(string(name)).filename();
        if (leaf.empty())
        {
            leaf = "capture";
        }
        leaf += string(CaptureExtension);
        return Diagnostics::CaptureDirectory() / leaf;
    }

    RecorderCore::RecorderCore(Hooks hooks) : m_Hooks(std::move(hooks))
    {
        if (!m_Hooks.Now)
        {
            m_Hooks.Now = DefaultNow;
        }
        if (!m_Hooks.WaitStep)
        {
            m_Hooks.WaitStep = DefaultWaitStep;
        }
    }

    RecorderCore::~RecorderCore()
    {
        // A core torn down mid-capture still owes the pool its buffers back; the file is whatever
        // was appended before, which is a shorter recording rather than a broken one.
        if (m_Backend)
        {
            for (const PendingFrame& frame : m_Outstanding)
            {
                m_Backend->ReleasePixelBuffer(frame.Buffer);
            }
        }
        m_Outstanding.clear();
    }

    VoidResult RecorderCore::Open(RecorderCoreInfo info)
    {
        if (m_Status != VideoCaptureStatus::Off)
        {
            return std::unexpected(m_Status == VideoCaptureStatus::Recording
                                       ? "A capture is already recording."
                                       : "The previous capture is still being written to disk.");
        }

        if (!info.Backend)
        {
            return std::unexpected("This build has no video encoder backend.");
        }

        if (const VoidResult valid = ValidateCaptureSettings(info.Settings, info.Open.Channels);
            !valid)
        {
            return std::unexpected(valid.error());
        }

        std::error_code created;
        std::filesystem::create_directories(info.Open.File.parent_path(), created);

        if (const VoidResult opened = info.Backend->Open(info.Open); !opened)
        {
            return std::unexpected(opened.error());
        }

        m_Backend = std::move(info.Backend);
        m_Settings = std::move(info.Settings);
        m_TapBlockFrames = info.TapBlockFrames;
        m_Channels = info.Open.Channels;
        m_Outstanding.clear();
        m_OriginSeconds = m_Hooks.Now();
        m_LastPts = -1;
        m_AudioFrames = 0;
        m_BudgetSpent = false;
        m_Finished.reset();
        m_Status = VideoCaptureStatus::Recording;

        m_State = VideoCaptureState{
            .Status = VideoCaptureStatus::Recording,
            .Lockstep = m_Settings.Lockstep,
            .Encoding = info.Open.Encoding,
            .IncludeOverlay = m_Settings.IncludeOverlay,
            .Codec = DescribeCodec(info.Open.Codec, info.Open.Encoding),
            .BitrateMbps = static_cast<u32>((info.Open.BitsPerSecond + 500000ULL) / 1000000ULL),
            .Extent = info.Open.Extent,
            .FrameBudget = m_Settings.FrameBudget,
            .Path = info.Open.File,
        };

        return {};
    }

    i64 RecorderCore::NextVideoPts(const f64 nowSeconds)
    {
        i64 ticks = 0;
        if (m_Settings.Lockstep)
        {
            const f64 rate = static_cast<f64>(m_Settings.FrameRate);
            ticks = std::llround(static_cast<f64>(m_State.FramesAcquired) *
                                 static_cast<f64>(VideoTimescale) / rate);
        }
        else
        {
            ticks = std::llround((nowSeconds - m_OriginSeconds) * static_cast<f64>(VideoTimescale));
        }

        ticks = std::max<i64>(ticks, 0);
        if (ticks <= m_LastPts)
        {
            ticks = m_LastPts + 1;
        }
        m_LastPts = ticks;
        return ticks;
    }

    optional<PixelBufferHandle> RecorderCore::Acquire(const u32 slot)
    {
        if (m_Status != VideoCaptureStatus::Recording)
        {
            return std::nullopt;
        }

        if (m_Settings.FrameBudget != 0 && m_State.FramesAcquired >= m_Settings.FrameBudget)
        {
            m_BudgetSpent = true;
            return std::nullopt;
        }

        const f64 waitStart = m_Hooks.Now();
        f64 nowSeconds = waitStart;
        PixelBufferHandle handle;
        bool waited = false;

        for (;;)
        {
            std::expected<PixelBufferHandle, AcquireRefusal> acquired =
                m_Backend->AcquirePixelBuffer();
            if (acquired)
            {
                handle = *acquired;
                break;
            }

            if (acquired.error().Reason == AcquireWait::WriterFailed)
            {
                Abort(acquired.error().Error.empty() ? string("the writer failed")
                                                     : acquired.error().Error);
                return std::nullopt;
            }

            // The frame waits rather than being dropped, so a real-time capture slows the
            // application instead of losing a picture — but the wait happens with the swap chain
            // image already acquired, so it is bounded and the capture ends rather than the app
            // stalling with its Stop control unreachable.
            nowSeconds = m_Hooks.Now();
            waited = true;
            if (nowSeconds - waitStart >= EncoderWaitBoundSeconds)
            {
                ++m_State.FramesWaited;
                m_State.WaitedForEncoderMs += (nowSeconds - waitStart) * 1000.0;
                Abort("encoder stalled");
                return std::nullopt;
            }

            m_Hooks.WaitStep();
        }

        if (waited)
        {
            ++m_State.FramesWaited;
            m_State.WaitedForEncoderMs += (nowSeconds - waitStart) * 1000.0;
        }

        const i64 pts = NextVideoPts(nowSeconds);
        m_Outstanding.emplace_back(PendingFrame{.Slot = slot, .Buffer = handle, .Pts = pts});
        ++m_State.FramesAcquired;

        if (m_Settings.FrameBudget != 0 && m_State.FramesAcquired >= m_Settings.FrameBudget)
        {
            m_BudgetSpent = true;
        }

        return handle;
    }

    void RecorderCore::AppendAndRelease(const PendingFrame& frame)
    {
        const VoidResult appended = m_Backend->AppendVideo(frame.Buffer, frame.Pts);

        // Released whether or not the append took: the pool vends the surface again only once the
        // recorder's own reference is gone, so holding a refused buffer would starve the pool.
        m_Backend->ReleasePixelBuffer(frame.Buffer);

        if (appended)
        {
            ++m_State.FramesAppended;
            m_State.DurationSeconds =
                static_cast<f64>(frame.Pts) / static_cast<f64>(VideoTimescale);
        }
        else if (m_State.LastError.empty())
        {
            m_State.LastError = appended.error();
        }
    }

    void RecorderCore::Retire(const u32 slot)
    {
        if (!m_Backend || m_Outstanding.empty())
        {
            return;
        }

        const auto found = std::ranges::find(m_Outstanding, slot, &PendingFrame::Slot);
        if (found == m_Outstanding.end())
        {
            return;
        }

        AppendAndRelease(*found);
        m_Outstanding.erase(found);
    }

    void RecorderCore::PushAudio(std::span<const f32> interleaved, const u32 frames,
                                 const u64 lostBlocks)
    {
        if (m_Status != VideoCaptureStatus::Recording || m_Settings.Audio == AudioTrack::None)
        {
            return;
        }

        if (lostBlocks > 0)
        {
            // The tap drops the newest block when its ring is full, so the mix has a hole. Writing
            // silence of the lost length is what keeps the sound track aligned with the picture.
            m_State.AudioOverruns += lostBlocks;
            const u64 lostFrames = lostBlocks * static_cast<u64>(m_TapBlockFrames);
            const vector<f32> silence(static_cast<usize>(lostFrames) * m_Channels, 0.0f);
            (void)m_Backend->AppendAudio(silence, static_cast<u32>(lostFrames), m_AudioFrames);
            m_AudioFrames += lostFrames;
        }

        if (frames == 0)
        {
            return;
        }

        if (const VoidResult appended = m_Backend->AppendAudio(interleaved, frames, m_AudioFrames);
            !appended && m_State.LastError.empty())
        {
            m_State.LastError = appended.error();
        }
        m_AudioFrames += frames;
        ++m_State.AudioBlocks;
    }

    void RecorderCore::Finish()
    {
        if (m_Status != VideoCaptureStatus::Recording)
        {
            return;
        }

        if (m_Hooks.BeforeDrain)
        {
            m_Hooks.BeforeDrain();
        }

        // Never waits for a future frame: every buffer still in flight is appended in the order it
        // was composited, so quitting or minimizing produces a complete file rather than a stall.
        for (const PendingFrame& frame : m_Outstanding)
        {
            AppendAndRelease(frame);
        }
        m_Outstanding.clear();

        m_Status = VideoCaptureStatus::Finalizing;
        m_Finished = std::make_shared<std::atomic<bool>>(false);

        const FinishFlag flag = m_Finished;
        m_Backend->Finish([flag] { flag->store(true, std::memory_order_release); });
    }

    void RecorderCore::Abort(string reason)
    {
        if (m_Status != VideoCaptureStatus::Recording)
        {
            return;
        }

        Log::Error("Video capture ended early: {}", reason);
        m_State.LastError = std::move(reason);
        Finish();
    }

    void RecorderCore::PollFinalize()
    {
        if (m_Status != VideoCaptureStatus::Finalizing)
        {
            return;
        }

        if (!m_Finished || !m_Finished->load(std::memory_order_acquire))
        {
            return;
        }

        if (m_Backend)
        {
            m_State.BytesWritten = m_Backend->FileBytes();
            m_Backend.reset();
        }
        m_Status = VideoCaptureStatus::Off;
    }

    void RecorderCore::WaitForFinalize()
    {
        while (m_Status == VideoCaptureStatus::Finalizing)
        {
            PollFinalize();
            if (m_Status != VideoCaptureStatus::Finalizing)
            {
                return;
            }

            // The wait is unbounded — a bounded one would leave a truncated file — so what ends a
            // wait that cannot complete is the writer's own failure, checked every iteration.
            if (m_Backend && m_Backend->IsWriterFailed())
            {
                if (m_State.LastError.empty())
                {
                    m_State.LastError = "the writer failed before the file was committed";
                }
                m_State.BytesWritten = m_Backend->FileBytes();
                m_Backend.reset();
                m_Status = VideoCaptureStatus::Off;
                return;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    VideoCaptureState RecorderCore::GetState() const
    {
        VideoCaptureState state = m_State;
        state.Status = m_Status;
        if (m_Backend)
        {
            state.BytesWritten = m_Backend->FileBytes();
        }
        return state;
    }
}
