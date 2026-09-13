#pragma once

#include <Veng/Veng.h>
#include <Veng/Path.h>
#include <Veng/Renderer/CaptureSink.h>

namespace Veng::Audio
{
    class AudioDevice;
}

namespace Veng::Renderer
{
    class Context;
    class ViewportCompositor;
}

/// @brief Recording what the application presents to a video file through the platform's encoder.
///
/// The presented frame is composited a second time into memory the encoder reads (Renderer::CaptureSink),
/// so nothing is copied through the CPU and the file's encoding is a setting rather than a consequence
/// of the display. The subsystem's design, the encoding table and what the test band can and cannot
/// prove are in engine/src/Capture/CLAUDE.md.
namespace Veng::Capture
{
    class RecorderCore;

    /// @brief How the recorded file is encoded: its pixel format, transfer function and colour tags.
    enum class CaptureEncoding : u8
    {
        /// @brief Resolve from the display: Hdr10 on an HDR10 swap chain, Sdr on any other.
        Auto,
        /// @brief 8-bit BGRA with the sRGB transfer function and BT.709 primaries.
        Sdr,
        /// @brief Ten-bit BT.2020 primaries with the SMPTE ST 2084 (PQ) transfer function.
        Hdr10,
    };

    /// @brief The video codec the file's picture track is encoded with.
    enum class VideoCodec : u8
    {
        /// @brief HEVC (H.265): Main for Sdr, Main 10 for Hdr10.
        Hevc,
        /// @brief H.264 High profile. Eight-bit only — it refuses Hdr10.
        H264,
        /// @brief Apple ProRes 422 HQ: intra-only, large, edit-friendly.
        ProRes422HQ,
        /// @brief Apple ProRes 4444: intra-only, the widest gamut of the four.
        ProRes4444,
    };

    /// @brief Whether the file carries a sound track, and how it is stored.
    enum class AudioTrack : u8
    {
        /// @brief No audio track; the device's block tap is never installed.
        None,
        /// @brief Uncompressed interleaved 32-bit float PCM.
        Pcm,
        /// @brief AAC, encoded by the writer.
        Aac,
    };

    /// @brief Everything a capture is started with.
    struct VideoCaptureSettings
    {
        /// @brief The file's encoding; Auto follows the display.
        CaptureEncoding Encoding = CaptureEncoding::Auto;

        /// @brief The picture track's codec.
        VideoCodec Codec = VideoCodec::Hevc;

        /// @brief Quality for the bitrate-denominated codecs, in bits per pixel per frame.
        ///
        /// The bitrate is extent.x * extent.y * FrameRate * BitsPerPixel, so one quality serves a
        /// small window and a Retina full screen alike. Ignored by ProRes, which is not
        /// rate-controlled.
        f32 BitsPerPixel = 0.15f;

        /// @brief Explicit bitrate in megabits per second, overriding BitsPerPixel; 0 derives it.
        u32 BitrateMbps = 0;

        /// @brief Whether the frame clock is driven for the capture's duration.
        ///
        /// Driven, every frame is simulated, rendered and encoded before the next begins, so the
        /// file plays back as if the machine had rendered at FrameRate however slowly it actually
        /// did. The audio device is driven with it, so the sound track is sample-locked to the
        /// picture and nothing is emitted to the speakers for the span.
        bool Lockstep = false;

        /// @brief The file's frame rate: the driven clock's rate in lockstep, the nominal rate the
        ///        bitrate is derived at in real time.
        u32 FrameRate = 60;

        /// @brief Stop after this many frames have been composited; 0 records until Stop.
        u64 FrameBudget = 0;

        /// @brief Whether and how the mixed audio is muxed into the file.
        AudioTrack Audio = AudioTrack::Pcm;

        /// @brief Whether the application's own overlay is composited into the recording.
        ///
        /// The overlay is the application's chrome — a debug shell, an editor's panels. A game's HUD
        /// and menus are driven into the viewports and are recorded either way.
        bool IncludeOverlay = false;

        /// @brief The capture's base name; empty yields "<app>-<yyyymmdd-hhmmss>".
        ///
        /// Resolved under Diagnostics::CaptureDirectory() with the QuickTime extension; only the
        /// name's final path component is taken, so a capture never escapes that directory.
        string Name;
    };

    /// @brief What the recorder is doing right now.
    enum class VideoCaptureStatus : u8
    {
        /// @brief Not recording: no sink is installed and no file is open.
        Off,
        /// @brief Frames are being composited into the writer's buffers and appended.
        Recording,
        /// @brief Recording has ended and the writer is still committing the file to disk.
        Finalizing,
    };

    /// @brief A snapshot of the recorder's state, so a caller can report honestly.
    struct VideoCaptureState
    {
        /// @brief What the recorder is doing right now.
        VideoCaptureStatus Status = VideoCaptureStatus::Off;
        /// @brief Whether the running (or last) capture drove the frame clock.
        bool Lockstep = false;
        /// @brief The encoding the capture resolved to; never Auto.
        CaptureEncoding Encoding = CaptureEncoding::Sdr;
        /// @brief Whether the application's overlay is composited into the recording.
        bool IncludeOverlay = false;
        /// @brief The codec and profile in words, e.g. "HEVC Main 10".
        string Codec;
        /// @brief The resolved bitrate in megabits per second; 0 for a codec that is not rate-controlled.
        u32 BitrateMbps = 0;
        /// @brief The recorded frame size in pixels.
        uvec2 Extent{0, 0};
        /// @brief Frames composited into a writer buffer since Start.
        u64 FramesAcquired = 0;
        /// @brief Frames handed to the writer since Start; trails FramesAcquired by the frames in flight.
        u64 FramesAppended = 0;
        /// @brief The settings' frame budget, echoed so a caller can show progress; 0 for unbounded.
        u64 FrameBudget = 0;
        /// @brief File time of the most recently appended frame, in seconds.
        f64 DurationSeconds = 0.0;
        /// @brief Frames that waited on the encoder rather than being dropped.
        u64 FramesWaited = 0;
        /// @brief Total time spent waiting on the encoder, in milliseconds.
        f64 WaitedForEncoderMs = 0.0;
        /// @brief Mixed audio blocks appended to the sound track.
        u64 AudioBlocks = 0;
        /// @brief Mixed audio blocks the tap ring dropped, each written to the file as silence.
        u64 AudioOverruns = 0;
        /// @brief The file's size on disk, polled when the state is read.
        u64 BytesWritten = 0;
        /// @brief The running capture's file, or the last one's; empty before the first Start.
        path Path;
        /// @brief Why Start refused, or why a capture ended early; empty when nothing went wrong.
        string LastError;
    };

    /// @brief Whether @p codec can encode @p encoding.
    ///
    /// H.264's hardware encoder has no ten-bit profile, so it refuses Hdr10; every other pairing is
    /// supported. Start applies this, and a caller offering the choice applies it first so an
    /// impossible pairing is never offered.
    /// @param codec     The picture codec.
    /// @param encoding  The encoding, resolved or Auto (which every codec supports).
    /// @return True when the pairing can be encoded.
    [[nodiscard]] bool CodecSupportsEncoding(VideoCodec codec, CaptureEncoding encoding);

    /// @brief Names @p codec at @p encoding the way the state reports it, e.g. "HEVC Main 10".
    /// @param codec     The picture codec.
    /// @param encoding  The resolved encoding.
    /// @return The codec and profile in words.
    [[nodiscard]] string DescribeCodec(VideoCodec codec, CaptureEncoding encoding);

    /// @brief The host services the recorder reaches back into, supplied by whoever owns it.
    ///
    /// The recorder drives the host's frame clock for a lockstep capture and names its files after
    /// the host, and takes both as plain values so it is constructible without the host's own type.
    struct VideoRecorderHost
    {
        /// @brief Drives the host's frame clock at a fixed delta in seconds; empty refuses Lockstep.
        function<void(f32 delta)> DriveFrameClock;

        /// @brief Returns the host's frame clock to wall time.
        function<void()> ReleaseFrameClock;

        /// @brief The host's name, used as the stem of a capture whose name was left empty.
        string Name = "veng";
    };

    /// @brief Records the presented frame to a video file through the platform's hardware encoder.
    ///
    /// The recorder is the compositor's capture sink: every frame it takes a fresh pixel buffer from
    /// the writer's pool, hands the composite the engine image that buffer's memory was imported as,
    /// and — at the frame slot's retirement, once its fence has been waited — appends the buffer to
    /// the writer and releases its own reference so the pool may vend the surface again. Nothing is
    /// copied through the CPU.
    ///
    /// A frame is never dropped: an encoder that has fallen behind makes the frame wait on the main
    /// thread, bounded, so a real-time capture slows the application rather than skipping a picture.
    /// Stop never waits for future frames — it drains what is outstanding and finalizes — so quitting,
    /// minimizing, or stopping on the last frame all produce a complete file.
    ///
    /// @warning Available only where the platform has both an encoder and texture interop, and only
    ///          on a windowed run. IsAvailable() reports it and Start refuses with a reason.
    class VideoRecorder final : public Renderer::CaptureSink
    {
    public:
        /// @brief Constructs the recorder over the services it records from.
        ///
        /// Subscribes to the context's swap-chain invalidation once, so any later recreation — a
        /// resize, or a display change moving the format or colour space — ends a running capture
        /// rather than letting the file's frame size or encoding change under it.
        /// @param context     The render context; must outlive the recorder.
        /// @param compositor  The compositor the recorder installs itself on as a capture sink.
        /// @param device      The audio device whose mixed blocks become the sound track.
        /// @param host        The host's frame clock and name.
        VideoRecorder(Renderer::Context& context, Renderer::ViewportCompositor& compositor,
                      Audio::AudioDevice& device, VideoRecorderHost host);

        /// @brief Stops a running capture, drains it, and waits for the file to finish.
        ~VideoRecorder() override;

        VideoRecorder(const VideoRecorder&) = delete;
        VideoRecorder& operator=(const VideoRecorder&) = delete;

        /// @brief Whether this build and this run can record at all.
        ///
        /// True only with a platform backend compiled in, external texture import supported by the
        /// device, and a live swap chain — a headless run has no presented frame to record.
        [[nodiscard]] bool IsAvailable() const;

        /// @brief Begins a capture, or refuses and says why.
        ///
        /// Validates the settings, resolves Auto against the display, opens the writer at the swap
        /// chain's extent, installs itself as the compositor's capture sink, installs the audio
        /// device's block tap, and — for a lockstep capture — drives the host's frame clock. A
        /// refusal leaves the recorder Off with the reason in GetState().LastError.
        /// @param settings  The capture's settings.
        /// @return True when the capture started.
        bool Start(const VideoCaptureSettings& settings);

        /// @brief Ends a running capture and finalizes the file.
        ///
        /// Waits the device idle, appends every frame still in flight in order, releases them, marks
        /// the track inputs finished and asks the writer to commit. The status stays Finalizing until
        /// the writer's completion runs; the file is complete to its last appended frame either way.
        /// A no-op when nothing is recording.
        void Stop();

        /// @brief Blocks until a finalizing file has been committed to disk.
        ///
        /// Unbounded by design: a bounded wait would leave a truncated file. A writer that has failed
        /// ends the wait, since it will commit nothing further. Returns immediately when the recorder
        /// is not finalizing.
        void WaitForFinalize();

        /// @brief Whether a capture is running (not merely finalizing).
        [[nodiscard]] bool IsRecording() const;

        /// @brief Returns the current state, polling the file's size on disk.
        [[nodiscard]] VideoCaptureState GetState() const;

        /// @brief Returns the target this frame's capture composite renders into.
        ///
        /// Takes a fresh pixel buffer from the writer's pool — waiting, bounded, on an encoder that
        /// has fallen behind — imports it through a cache keyed by the buffer's surface, and records
        /// the slot the buffer belongs to. Returns nullopt when nothing is recording, when the frame
        /// budget is spent, or when the wait ended the capture.
        /// @param slot            The frame-in-flight slot this frame records into.
        /// @param presentedExtent The extent the presented frame is composited at.
        /// @return The target to composite into, or nullopt.
        optional<Renderer::CaptureTarget> AcquireTarget(u32 slot, uvec2 presentedExtent) override;

        /// @brief Appends the buffer the retired slot was given and releases the recorder's reference.
        ///
        /// The slot's fence has been waited, so everything the capture composite wrote is in the
        /// buffer's memory and the encoder may read it. A slot the recorder gave no buffer is ignored.
        /// @param slot  The frame-in-flight slot that retired.
        void OnSlotRetired(u32 slot) override;

    private:
        /// @brief Detaches the sink, the audio tap and the driven clock, leaving the file finalizing.
        void Detach();

        /// @brief Ends a running capture with @p reason, draining the file intact to its last frame.
        /// @param reason  What went wrong, reported as VideoCaptureState::LastError.
        void Abort(string reason);

        /// @brief Resolves CaptureEncoding::Auto against the display's resolved colour space.
        /// @param requested  The requested encoding.
        /// @return The requested encoding, or the display's for Auto.
        [[nodiscard]] CaptureEncoding ResolveEncoding(CaptureEncoding requested) const;

        /// @brief The render context the frame is composited by; borrowed.
        Renderer::Context& m_Context;

        /// @brief The compositor this recorder installs itself on while recording; borrowed.
        Renderer::ViewportCompositor& m_Compositor;

        /// @brief The audio device whose mixed blocks become the sound track; borrowed.
        Audio::AudioDevice& m_Device;

        /// @brief The host's frame clock and name.
        VideoRecorderHost m_Host;

        /// @brief The device-free half: the backend, the slot map, the timestamps and the state.
        Unique<RecorderCore> m_Core;

        /// @brief Engine images wrapping the writer's pixel buffers, keyed by the buffer's surface.
        ///
        /// A pool recycles a handful of surfaces across a whole capture, so a recycled buffer pays
        /// the import once rather than every frame it comes back around.
        map<void*, Ref<Renderer::Image>> m_Imports;

        /// @brief The tap overrun count read at the previous audio block; the delta is the loss.
        u64 m_LastTapOverruns = 0;

        /// @brief The picture size the writer was opened at; a presented frame of any other size ends it.
        uvec2 m_Extent{0, 0};

        /// @brief Why the most recent Start refused; empty when the last one was accepted.
        string m_StartError;

        /// @brief Whether this recorder is installed as the compositor's capture sink.
        bool m_SinkInstalled = false;

        /// @brief Whether this recorder holds the audio device's block tap.
        bool m_TapInstalled = false;

        /// @brief Whether this recorder is the one that drove the host's frame clock.
        bool m_DroveFrameClock = false;

        /// @brief Whether a capture that ended inside the composite still owes its detach.
        ///
        /// Removing the sink from inside AcquireTarget would mutate the compositor mid-composite, so
        /// a capture that ends there is detached at the next frame's slot retirement instead.
        bool m_NeedsDetach = false;
    };
}
