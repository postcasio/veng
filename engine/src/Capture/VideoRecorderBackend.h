#pragma once

// The narrow seam between the recorder's device-free logic and the one platform that has a
// hardware encoder. Everything above this header is portable: the backend deals in opaque handles,
// so no Objective-C, AVFoundation or Core Video type reaches the recorder, its tests, or the
// public API.

#include <Veng/Capture/VideoRecorder.h>
#include <Veng/Path.h>
#include <Veng/Result.h>
#include <Veng/Veng.h>

#include <expected>
#include <span>

namespace Veng::Capture
{
    /// @brief The timescale every video presentation timestamp is expressed in, in ticks per second.
    ///
    /// A fixed integer timescale keeps the strictly-increasing requirement exact: a clamp to the
    /// previous timestamp plus one tick is unambiguous where a floating-point time is not. 60 000 is
    /// divisible by every common frame rate, so a lockstep capture's timestamps are exact.
    constexpr i64 VideoTimescale = 60000;

    /// @brief One pixel buffer taken from the writer's pool, as the recorder holds it.
    ///
    /// The recorder never dereferences any of these; it hands Texture to the import, keys its import
    /// cache on Surface, and gives the whole handle back to the backend to append and release.
    struct PixelBufferHandle
    {
        /// @brief The platform texture wrapping the buffer's memory, for the import.
        void* Texture = nullptr;
        /// @brief The buffer's shared-surface identity, the import cache's key.
        void* Surface = nullptr;
        /// @brief The pixel buffer itself, which only the backend interprets.
        void* Buffer = nullptr;

        /// @brief Whether the handle names a buffer.
        [[nodiscard]] bool IsValid() const { return Buffer != nullptr; }
    };

    /// @brief Why the backend could not vend a buffer this frame.
    enum class AcquireWait : u8
    {
        /// @brief The pool is at its allocation ceiling: the encoder holds every buffer it vended.
        PoolAtThreshold,
        /// @brief The writer is not accepting more picture data yet.
        WriterNotReady,
        /// @brief The writer has failed; the capture cannot continue.
        WriterFailed,
    };

    /// @brief A refused acquire: why, and the writer's own words when it failed.
    struct AcquireRefusal
    {
        /// @brief Why the backend refused.
        AcquireWait Reason = AcquireWait::WriterNotReady;
        /// @brief The writer's error description; empty unless Reason is WriterFailed.
        string Error;
    };

    /// @brief Everything the backend needs to open a writer.
    struct BackendOpenInfo
    {
        /// @brief The file to write; its directory is created if missing and an existing file replaced.
        path File;
        /// @brief The picture size in pixels.
        uvec2 Extent{0, 0};
        /// @brief The platform graphics device the buffers' textures must be created on.
        ///
        /// A texture created on any other device cannot be imported by the renderer, so this is the
        /// renderer's own device (Renderer::Context::GetExternalDevice()).
        void* Device = nullptr;
        /// @brief The resolved encoding, which fixes the pool's pixel format and the colour tags.
        CaptureEncoding Encoding = CaptureEncoding::Sdr;
        /// @brief The picture codec.
        VideoCodec Codec = VideoCodec::Hevc;
        /// @brief The average bitrate in bits per second; ignored by a codec that is not rate-controlled.
        u64 BitsPerSecond = 0;
        /// @brief The file's nominal frame rate.
        u32 FrameRate = 60;
        /// @brief Whether frames arrive at wall cadence, which the writer paces itself against.
        bool RealTime = true;
        /// @brief Whether and how a sound track is written.
        AudioTrack Audio = AudioTrack::None;
        /// @brief The sound track's sample rate in Hz.
        u32 SampleRate = 48000;
        /// @brief The sound track's channel count; one or two.
        u32 Channels = 2;
        /// @brief Buffers the pool may have outstanding before it refuses to vend another.
        ///
        /// The back-pressure that turns an encoder falling behind into a bounded wait rather than
        /// unbounded surface growth — a 4K buffer is some thirty megabytes.
        u32 PoolAllocationThreshold = 8;
    };

    /// @brief The platform half of the recorder: one writer, its buffer pool, and its two track inputs.
    ///
    /// Every call happens on the main thread, in the order the recorder makes them. The one exception
    /// is Finish's completion, which the platform runs on a thread of its own.
    class VideoRecorderBackend
    {
    public:
        /// @brief Destroys the backend, releasing the writer and any cached per-buffer resources.
        virtual ~VideoRecorderBackend() = default;

        /// @brief Opens the writer and starts its session, or explains why it could not.
        /// @param info  The writer's descriptor.
        /// @return Nothing on success, else a human-readable reason.
        virtual VoidResult Open(const BackendOpenInfo& info) = 0;

        /// @brief Takes a fresh buffer from the writer's pool.
        ///
        /// Fresh every time: the pool vends a surface again only once both the encoder and the
        /// recorder have released the previous buffer holding it, which is what makes the recycling
        /// safe.
        /// @return The buffer, or why the caller must wait (or stop).
        virtual std::expected<PixelBufferHandle, AcquireRefusal> AcquirePixelBuffer() = 0;

        /// @brief Appends a written buffer to the picture track.
        /// @param handle    A buffer from AcquirePixelBuffer whose memory the GPU has finished writing.
        /// @param ptsTicks  The presentation timestamp in VideoTimescale ticks; strictly increasing.
        /// @return Nothing on success, else the writer's reason.
        virtual VoidResult AppendVideo(const PixelBufferHandle& handle, i64 ptsTicks) = 0;

        /// @brief Releases the recorder's reference to a buffer, returning it to the pool's care.
        /// @param handle  A buffer from AcquirePixelBuffer.
        virtual void ReleasePixelBuffer(const PixelBufferHandle& handle) = 0;

        /// @brief Appends one block of interleaved float samples to the sound track.
        /// @param interleaved  Channels() interleaved samples per frame.
        /// @param frames       Number of sample frames in @p interleaved.
        /// @param ptsFrames    The block's first frame's index from the capture's origin.
        /// @return Nothing on success, else the writer's reason.
        virtual VoidResult AppendAudio(std::span<const f32> interleaved, u32 frames,
                                       u64 ptsFrames) = 0;

        /// @brief Marks the tracks finished and commits the file, calling @p completion when it lands.
        ///
        /// The completion runs on a platform thread and must touch nothing the backend owns, since
        /// the backend may be destroyed before it fires.
        /// @param completion  Invoked once the file has been committed, successfully or not.
        virtual void Finish(function<void()> completion) = 0;

        /// @brief Returns the file's current size on disk in bytes; 0 before anything is written.
        [[nodiscard]] virtual u64 FileBytes() const = 0;

        /// @brief Whether the writer has failed, so nothing further will ever be committed.
        [[nodiscard]] virtual bool IsWriterFailed() const = 0;
    };

    /// @brief Creates the platform backend, or null where this build has none.
    [[nodiscard]] Unique<VideoRecorderBackend> CreateVideoRecorderBackend();

    /// @brief Whether a platform backend is compiled into this build at all.
    [[nodiscard]] bool IsVideoRecorderBackendCompiled();
}
