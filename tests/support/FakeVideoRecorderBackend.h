#pragma once

// A recording stand-in for the platform's encoder, so the recorder's whole device-free half — the
// slot map, the timestamps, the never-drop wait, the drain and the audio alignment — is drivable
// from the unit band with no device, no encoder and no file. It records what it was asked to do and
// exposes knobs for the two things only a real encoder does on its own: falling behind, and failing.

#include <Capture/VideoRecorderBackend.h>

#include <Veng/Veng.h>

#include <span>

namespace Veng::Test
{
    /// @brief A VideoRecorderBackend that records its calls instead of encoding them.
    class FakeVideoRecorderBackend final : public Capture::VideoRecorderBackend
    {
    public:
        /// @brief Whether the writer accepts more picture data; false refuses every acquire.
        bool Ready = true;

        /// @brief When non-empty, every acquire refuses as a failed writer with this reason.
        string FailWith;

        /// @brief Whether IsWriterFailed reports a failure.
        bool Failed = false;

        /// @brief The descriptor Open was handed.
        Capture::BackendOpenInfo OpenedWith;

        /// @brief Timestamps appended to the picture track, in append order.
        vector<i64> VideoPts;

        /// @brief Buffers appended to the picture track, in append order.
        vector<void*> VideoBuffers;

        /// @brief Timestamps appended to the sound track, in append order.
        vector<u64> AudioPts;

        /// @brief Sample frames appended to the sound track, in append order.
        vector<u32> AudioFrames;

        /// @brief Total sample frames appended to the sound track.
        u64 TotalAudioFrames = 0;

        /// @brief Total sample values appended to the sound track that were not zero.
        u64 NonSilentSamples = 0;

        /// @brief Times each buffer was appended, keyed by buffer.
        map<void*, u32> Appends;

        /// @brief Times each buffer was released, keyed by buffer.
        map<void*, u32> Releases;

        /// @brief Whether every release so far followed exactly one append of that buffer.
        bool ReleasedAfterAppend = true;

        /// @brief Buffers handed out and not yet released.
        u32 Outstanding = 0;

        /// @brief Whether Finish has run.
        bool Finished = false;

        /// @brief The file size FileBytes reports.
        u64 Bytes = 0;

        VoidResult Open(const Capture::BackendOpenInfo& info) override
        {
            OpenedWith = info;
            return {};
        }

        std::expected<Capture::PixelBufferHandle, Capture::AcquireRefusal>
        AcquirePixelBuffer() override
        {
            if (!FailWith.empty())
            {
                return std::unexpected(Capture::AcquireRefusal{
                    .Reason = Capture::AcquireWait::WriterFailed, .Error = FailWith});
            }
            if (!Ready)
            {
                return std::unexpected(
                    Capture::AcquireRefusal{.Reason = Capture::AcquireWait::WriterNotReady});
            }

            // The identity is all the recorder does with a buffer, so a counter serves as one.
            void* const buffer = reinterpret_cast<void*>(m_NextBuffer++);
            ++Outstanding;
            return Capture::PixelBufferHandle{
                .Texture = buffer, .Surface = buffer, .Buffer = buffer};
        }

        VoidResult AppendVideo(const Capture::PixelBufferHandle& handle,
                               const i64 ptsTicks) override
        {
            VideoPts.push_back(ptsTicks);
            VideoBuffers.push_back(handle.Buffer);
            ++Appends[handle.Buffer];
            return {};
        }

        void ReleasePixelBuffer(const Capture::PixelBufferHandle& handle) override
        {
            ReleasedAfterAppend =
                ReleasedAfterAppend && Appends[handle.Buffer] == 1 && Releases[handle.Buffer] == 0;
            ++Releases[handle.Buffer];
            --Outstanding;
        }

        VoidResult AppendAudio(std::span<const f32> interleaved, const u32 frames,
                               const u64 ptsFrames) override
        {
            AudioPts.push_back(ptsFrames);
            AudioFrames.push_back(frames);
            TotalAudioFrames += frames;
            for (const f32 sample : interleaved)
            {
                NonSilentSamples += sample != 0.0f ? 1 : 0;
            }
            return {};
        }

        void Finish(function<void()> completion) override
        {
            Finished = true;
            completion();
        }

        [[nodiscard]] u64 FileBytes() const override { return Bytes; }

        [[nodiscard]] bool IsWriterFailed() const override { return Failed; }

    private:
        /// @brief The next buffer identity to hand out; never dereferenced.
        usize m_NextBuffer = 1;
    };
}
