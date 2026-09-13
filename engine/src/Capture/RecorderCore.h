#pragma once

// The recorder's device-free half: the settings validation, the slot map, the timestamps, the
// never-drop wait, the drain, and the state a caller reports. It talks to a VideoRecorderBackend and
// to nothing else, so the unit band drives the whole of it against a fake backend and an injected
// clock — no device, no encoder, no file.

#include "VideoRecorderBackend.h"

#include <Veng/Capture/VideoRecorder.h>
#include <Veng/Path.h>
#include <Veng/Result.h>
#include <Veng/Veng.h>

#include <atomic>
#include <ctime>
#include <span>

namespace Veng::Capture
{
    /// @brief Checks a capture's settings against what can actually be encoded.
    ///
    /// The rules a caller offering the settings applies before offering them, and Start applies
    /// again: a codec that cannot carry the encoding, a zero frame rate, a non-positive quality with
    /// no bitrate override, and a channel count the writer has no layout for.
    /// @param settings  The settings to check.
    /// @param channels  The audio device's channel count, checked only when a sound track is asked for.
    /// @return Nothing when the settings are encodable, else the reason they are not.
    [[nodiscard]] VoidResult ValidateCaptureSettings(const VideoCaptureSettings& settings,
                                                     u32 channels);

    /// @brief Resolves a capture's average bitrate in bits per second.
    ///
    /// BitrateMbps wins when set; otherwise the rate is extent.x * extent.y * FrameRate *
    /// BitsPerPixel, so one quality figure serves every window size.
    /// @param settings  The capture's settings.
    /// @param extent    The picture size in pixels.
    /// @return The average bitrate in bits per second.
    [[nodiscard]] u64 DeriveBitsPerSecond(const VideoCaptureSettings& settings, uvec2 extent);

    /// @brief Builds the default capture name, "<app>-<yyyymmdd-hhmmss>".
    /// @param appName  The host application's name; non-alphanumerics become hyphens.
    /// @param when     The wall-clock time to stamp, in local time.
    /// @return The name, with no extension and no directory.
    [[nodiscard]] string DefaultCaptureName(string_view appName, std::time_t when);

    /// @brief Resolves a capture name to a file under Diagnostics::CaptureDirectory().
    ///
    /// Only the name's final path component is taken, so a name carrying separators cannot escape the
    /// capture directory — the same rule the profiler's captures follow.
    /// @param name  The capture's base name, without extension.
    /// @return The resolved path, with the QuickTime extension.
    [[nodiscard]] path ResolveVideoCapturePath(string_view name);

    /// @brief Everything RecorderCore::Open needs: the backend, the settings, and the writer's descriptor.
    struct RecorderCoreInfo
    {
        /// @brief The backend the core drives; ownership passes to the core.
        Unique<VideoRecorderBackend> Backend;
        /// @brief The capture's settings, with Encoding already resolved.
        VideoCaptureSettings Settings;
        /// @brief The writer's descriptor, which the core forwards to the backend unchanged.
        BackendOpenInfo Open;
        /// @brief Frames one dropped audio block stands for, written to the file as silence.
        u32 TapBlockFrames = 1024;
    };

    /// @brief Drives a VideoRecorderBackend: the slot map, the timestamps, the wait, and the drain.
    class RecorderCore
    {
    public:
        /// @brief The clock and the wait step, injected so the band drives both.
        struct Hooks
        {
            /// @brief Returns the wall-clock time in seconds on any fixed origin.
            function<f64()> Now;
            /// @brief Yields for a moment between two attempts at the encoder's pool.
            function<void()> WaitStep;
            /// @brief Runs before the drain, so the caller can make the outstanding buffers readable.
            ///
            /// The frames still in flight were written by command buffers whose fences the caller
            /// alone can wait; the core appends whatever this leaves behind.
            function<void()> BeforeDrain;
        };

        /// @brief How long a frame may wait on the encoder before the capture ends, in seconds.
        ///
        /// The wait happens with the swap chain image already acquired, so an unbounded one would
        /// become a presentation stall with the Stop control unreachable.
        static constexpr f64 EncoderWaitBoundSeconds = 2.0;

        /// @brief Constructs an idle core.
        /// @param hooks  The clock and wait step; an empty member takes its default.
        explicit RecorderCore(Hooks hooks = {});

        /// @brief Destroys the core, releasing any buffer still outstanding.
        ~RecorderCore();

        RecorderCore(const RecorderCore&) = delete;
        RecorderCore& operator=(const RecorderCore&) = delete;

        /// @brief Opens the backend and begins recording, or refuses and says why.
        /// @param info  The backend, the settings and the writer's descriptor.
        /// @return Nothing when recording began, else the reason it did not.
        VoidResult Open(RecorderCoreInfo info);

        /// @brief Takes this frame's buffer for @p slot, waiting on the encoder rather than dropping it.
        ///
        /// Returns nullopt when nothing is recording, when the frame budget is spent, or when the
        /// wait ended the capture — the only paths on which a frame is not recorded.
        /// @param slot  The frame-in-flight slot this frame records into.
        /// @return The buffer to composite into, or nullopt.
        [[nodiscard]] optional<PixelBufferHandle> Acquire(u32 slot);

        /// @brief Appends the buffer @p slot was given and releases it; ignores a slot with none.
        /// @param slot  The frame-in-flight slot whose fence has been waited.
        void Retire(u32 slot);

        /// @brief Appends one mixed block to the sound track, preceded by silence for any lost blocks.
        /// @param interleaved  Channels interleaved samples per frame.
        /// @param frames       Sample frames in @p interleaved.
        /// @param lostBlocks   Blocks the tap dropped since the previous call.
        void PushAudio(std::span<const f32> interleaved, u32 frames, u64 lostBlocks);

        /// @brief Ends the capture: appends every outstanding buffer in order and commits the file.
        ///
        /// Never waits for a future frame. The caller is responsible for the device being idle, so
        /// that what the outstanding buffers hold is what the GPU finished writing.
        void Finish();

        /// @brief Ends the capture with @p reason, leaving the file intact to its last appended frame.
        /// @param reason  What went wrong, reported as VideoCaptureState::LastError.
        void Abort(string reason);

        /// @brief Moves the status from Finalizing to Off once the writer has committed the file.
        void PollFinalize();

        /// @brief Blocks until a finalizing file has been committed, or its writer has failed.
        void WaitForFinalize();

        /// @brief Whether a capture is running.
        [[nodiscard]] bool IsRecording() const { return m_Status == VideoCaptureStatus::Recording; }

        /// @brief Whether the file is still being committed.
        [[nodiscard]] bool IsFinalizing() const
        {
            return m_Status == VideoCaptureStatus::Finalizing;
        }

        /// @brief Whether the frame budget is spent, so the caller should Finish at its next chance.
        [[nodiscard]] bool WantsStop() const { return m_BudgetSpent; }

        /// @brief Returns the current state, polling the file's size on disk.
        [[nodiscard]] VideoCaptureState GetState() const;

        /// @brief Returns the settings the running (or last) capture was opened with.
        [[nodiscard]] const VideoCaptureSettings& GetSettings() const { return m_Settings; }

    private:
        /// @brief One frame composited into a buffer, awaiting its slot's retirement.
        struct PendingFrame
        {
            /// @brief The frame-in-flight slot the frame recorded into.
            u32 Slot = 0;
            /// @brief The buffer the frame was composited into.
            PixelBufferHandle Buffer;
            /// @brief The frame's presentation timestamp in VideoTimescale ticks.
            i64 Pts = 0;
        };

        /// @brief The atomic the writer's completion sets, held by shared ownership.
        ///
        /// The completion outlives the core when a recorder is destroyed mid-commit, so it captures
        /// this and touches nothing else.
        using FinishFlag = Ref<std::atomic<bool>>;

        /// @brief Computes this frame's presentation timestamp, keeping the sequence strictly increasing.
        ///
        /// A lockstep capture stamps k / FrameRate from the recorder's own acquire count; a real-time
        /// one stamps the wall time the frame was taken at, rebased to the capture's origin. Either
        /// way a timestamp that did not advance is pushed to the previous one plus a tick, which the
        /// writer requires.
        /// @param nowSeconds  The wall-clock second this frame's buffer was taken at.
        /// @return The timestamp in VideoTimescale ticks.
        [[nodiscard]] i64 NextVideoPts(f64 nowSeconds);

        /// @brief Appends @p frame to the picture track and releases its buffer.
        /// @param frame  The frame whose slot has retired.
        void AppendAndRelease(const PendingFrame& frame);

        /// @brief The backend; null until Open, and kept alive while the file is being committed.
        Unique<VideoRecorderBackend> m_Backend;

        /// @brief The clock and wait step.
        Hooks m_Hooks;

        /// @brief The settings the capture was opened with, Encoding resolved.
        VideoCaptureSettings m_Settings;

        /// @brief Frames one dropped audio block stands for.
        u32 m_TapBlockFrames = 1024;

        /// @brief The sound track's channel count, the stride of a tapped block.
        u32 m_Channels = 2;

        /// @brief What the recorder is doing right now.
        VideoCaptureStatus m_Status = VideoCaptureStatus::Off;

        /// @brief Frames composited into a buffer but not yet appended, in acquire order.
        vector<PendingFrame> m_Outstanding;

        /// @brief The wall-clock second the capture's timestamps are rebased to.
        f64 m_OriginSeconds = 0.0;

        /// @brief The most recent picture timestamp in ticks; -1 before the first frame.
        i64 m_LastPts = -1;

        /// @brief The running sound-track write position in sample frames from the capture's origin.
        u64 m_AudioFrames = 0;

        /// @brief Whether the frame budget has been reached.
        bool m_BudgetSpent = false;

        /// @brief The capture's reported state, accumulated as it runs.
        VideoCaptureState m_State;

        /// @brief The flag the writer's completion sets; null until Finish.
        FinishFlag m_Finished;
    };
}
