#pragma once

#include <Veng/Veng.h>
#include <Veng/Diagnostics/TraceSink.h>

// FileTraceSink — the TraceSink that writes the on-disk trace format (docs/trace-format.md) to a
// file. It receives completed chunks and string deltas from the profiler and produces the compact
// binary stream two independent decoders read.
//
// Two invariants shape the implementation: the recording thread never blocks on I/O, and a reader
// never sees a half-written file. Chunks are copied and handed to a dedicated writer thread that
// does the transcoding and encoding off the hot path, and the finished stream is committed to its
// final path atomically (a sibling temporary renamed into place), so a ring dump is a complete file
// at the instant it appears.

namespace Veng::Diagnostics
{
    /// @brief A TraceSink that writes the on-disk trace format to a file, off the recording thread.
    ///
    /// Attach it with Profiler::SetSink. Chunks are queued to a background writer as they arrive; the
    /// file is built in memory and committed atomically at OnClose, so no recording thread ever
    /// blocks on I/O and no reader observes a torn capture. The identity fields the stream carries
    /// (engine version, build configuration, git provenance, executable basename) are gathered by the
    /// sink itself; a caller supplies only the destination path, the capture mode, and — before close
    /// — the accounting counts.
    class FileTraceSink final : public TraceSink
    {
    public:
        /// @brief Creates a file sink writing to @p filePath, creating parent directories as needed.
        ///
        /// The capture path convention is `<build-dir>/captures/`, gitignored and disposable with the
        /// build tree, so an observatory scanning that location can order captures and attribute them
        /// to the build that produced them.
        /// @param filePath  Destination path; the parent directory is created if absent.
        /// @param ringDump  True for a ring-dump capture (full string table); false for a triggered
        ///                  stream (string deltas).
        /// @return The sink, never null; a write failure surfaces at close through the log.
        [[nodiscard]] static Unique<FileTraceSink> Create(const path& filePath,
                                                          bool ringDump = false);

        /// @brief Joins the writer thread and commits the file if OnClose was not already called.
        ~FileTraceSink() override;

        FileTraceSink(const FileTraceSink&) = delete;
        FileTraceSink& operator=(const FileTraceSink&) = delete;

        /// @brief Queues a completed chunk to the writer thread; returns without blocking on I/O.
        /// @param thread  The track the chunk was recorded on.
        /// @param data    The chunk bytes.
        /// @param bytes   Number of valid bytes at @p data.
        void OnChunk(ThreadId thread, const u8* data, usize bytes) override;

        /// @brief Queues newly interned strings to the writer thread.
        /// @param delta  The new string ids and their text.
        void OnStrings(const StringTableDelta& delta) override;

        /// @brief Queues a track's name and role to the writer thread, enriching its Track section.
        /// @param track  The track descriptor.
        void OnTrack(const TrackDescriptor& track) override;

        /// @brief Marks a flush boundary; the file is committed at close, so this is a no-op.
        void OnFlush() override;

        /// @brief Drains the writer thread, encodes the stream, and commits the file atomically.
        void OnClose() override;

        /// @brief Initiates a clean close without blocking: the writer trailers and commits the file off-thread.
        ///
        /// Unlike OnClose, this returns immediately rather than joining the writer, so a capture
        /// controller closing a large ring dump does not stall on the encode + I/O. The file is
        /// committed a moment later; poll HasFinishedWriting() to learn when. Idempotent — a second
        /// call (or a later OnClose) is a no-op once the sink is closing.
        void BeginClose();

        /// @brief Returns true once the writer thread has committed the file and exited.
        ///
        /// False until BeginClose (or OnClose) has been issued and the off-thread encode + atomic
        /// commit have completed. A capture controller polls this to know a BeginClose'd file is on
        /// disk before handing its path back.
        [[nodiscard]] bool HasFinishedWriting() const;

        /// @brief Records the dropped-event and dropped-thread counts the Accounting section carries.
        ///
        /// The profiler owns these counts; a capture controller reads them and stamps them here
        /// before close, so a truncated or lossy capture is visibly lossy in the file.
        /// @param droppedEvents   Events discarded on ring wraps.
        /// @param droppedThreads  Thread registrations refused for exceeding the max.
        void SetAccounting(u64 droppedEvents, u64 droppedThreads);

        /// @brief Returns the destination path.
        [[nodiscard]] const path& GetPath() const;

    private:
        /// @brief Constructs the sink; use Create.
        FileTraceSink();

        /// @brief Writer-thread state (queue, thread, accumulated stream); defined in the implementation.
        struct Impl;
        /// @brief The opaque writer-thread state.
        Unique<Impl> m_Impl;
    };
}
