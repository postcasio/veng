#pragma once

#include <Veng/Veng.h>

// TraceSink — the swappable seam between the profiler and whatever consumes its
// completed buffer chunks. The profiler writes events into per-thread buffers on
// the hot path and only ever touches a sink at a chunk boundary, so the sink's
// cost never rides an individual event. The on-disk file sink is a separate
// header; this one ships the two the mechanism needs: a null sink (the default,
// no-recording state) and a capturing sink that retains chunks in memory for
// tests.

namespace Veng::Diagnostics
{
    /// @brief Dense per-thread track identifier assigned at thread registration.
    ///
    /// Chunks are handed to the sink tagged with the thread that produced them.
    using ThreadId = u32;

    /// @brief Interned-string identifier carried by every event record.
    ///
    /// A record stores a name as an id into the profiler's string table; the sink
    /// receives the id→string mapping through OnStrings deltas and resolves records
    /// against it. Id 0 is reserved for "no name".
    using NameId = u32;

    /// @brief A batch of newly interned strings since the sink's previous OnStrings call.
    ///
    /// The strings are contiguous ids starting at FirstId (FirstId, FirstId+1, ...),
    /// so a sink appends them to its own table without gaps. The string_views point
    /// into the profiler's live string table; a sink that retains them past the call
    /// copies them.
    struct StringTableDelta
    {
        /// @brief Id of the first string in Strings; subsequent strings are consecutive.
        NameId FirstId = 0;
        /// @brief The new strings, in ascending id order from FirstId.
        vector<string_view> Strings;
    };

    /// @brief Abstract consumer of completed profiler buffer chunks and string deltas.
    ///
    /// Every method is called from the thread that owns the chunk (a recording
    /// thread at a chunk boundary, or the collector during a flush/close); an
    /// implementation must not assume a single caller thread. Chunk granularity is
    /// what keeps the sink off the hot path: a recording thread hands over a whole
    /// filled buffer at once rather than one event at a time.
    class TraceSink
    {
    public:
        virtual ~TraceSink() = default;

        /// @brief Receives one completed, self-contained buffer chunk.
        ///
        /// @param thread  The track the chunk was recorded on.
        /// @param data    The chunk bytes: a chunk header followed by its records.
        /// @param bytes   Number of valid bytes at @p data.
        virtual void OnChunk(ThreadId thread, const u8* data, usize bytes) = 0;

        /// @brief Receives the strings interned since the previous call.
        /// @param delta  The new string ids and their text.
        virtual void OnStrings(const StringTableDelta& delta) = 0;

        /// @brief Marks a flush boundary: every chunk outstanding at the flush has been delivered.
        virtual void OnFlush() {}

        /// @brief Marks the sink's final boundary; no further calls follow.
        virtual void OnClose() {}
    };

    /// @brief The default sink: discards everything. The profiler's no-recording resting state.
    class NullTraceSink final : public TraceSink
    {
    public:
        /// @brief Discards the chunk.
        void OnChunk(ThreadId /*thread*/, const u8* /*data*/, usize /*bytes*/) override {}
        /// @brief Discards the string delta.
        void OnStrings(const StringTableDelta& /*delta*/) override {}
    };

    /// @brief A sink that retains every chunk and string in memory, for the test band.
    ///
    /// Chunks are copied out of the profiler's buffers as they arrive, so they stay
    /// valid after the profiler recycles the underlying storage; strings are copied
    /// out of the transient delta views. Not for production capture — it grows
    /// without bound.
    class CapturingTestSink final : public TraceSink
    {
    public:
        /// @brief One retained chunk: the track it came from and a copy of its bytes.
        struct CapturedChunk
        {
            /// @brief The track the chunk was recorded on.
            ThreadId Thread = 0;
            /// @brief A copy of the chunk's bytes (header + records).
            vector<u8> Bytes;
        };

        /// @brief Copies the chunk into the retained list.
        void OnChunk(ThreadId thread, const u8* data, usize bytes) override
        {
            CapturedChunk chunk;
            chunk.Thread = thread;
            chunk.Bytes.assign(data, data + bytes);
            m_Chunks.push_back(std::move(chunk));
        }

        /// @brief Copies the new strings into the retained table at their ids.
        void OnStrings(const StringTableDelta& delta) override
        {
            if (m_Strings.size() < static_cast<usize>(delta.FirstId) + delta.Strings.size())
            {
                m_Strings.resize(static_cast<usize>(delta.FirstId) + delta.Strings.size());
            }
            for (usize i = 0; i < delta.Strings.size(); ++i)
            {
                m_Strings[delta.FirstId + i] = string(delta.Strings[i]);
            }
        }

        /// @brief Counts a flush boundary.
        void OnFlush() override { ++m_FlushCount; }
        /// @brief Counts the close boundary.
        void OnClose() override { ++m_CloseCount; }

        /// @brief Returns the retained chunks, in arrival order.
        [[nodiscard]] const vector<CapturedChunk>& GetChunks() const { return m_Chunks; }
        /// @brief Returns the retained string for an id, or empty if unknown.
        [[nodiscard]] string_view GetString(NameId id) const
        {
            return id < m_Strings.size() ? string_view(m_Strings[id]) : string_view();
        }
        /// @brief Returns the number of strings retained.
        [[nodiscard]] usize GetStringCount() const { return m_Strings.size(); }
        /// @brief Returns how many times OnFlush was called.
        [[nodiscard]] u32 GetFlushCount() const { return m_FlushCount; }
        /// @brief Returns how many times OnClose was called.
        [[nodiscard]] u32 GetCloseCount() const { return m_CloseCount; }
        /// @brief Drops every retained chunk (strings and boundary counts are kept).
        void ClearChunks() { m_Chunks.clear(); }

    private:
        /// @brief Retained chunk copies, in arrival order.
        vector<CapturedChunk> m_Chunks;
        /// @brief Retained strings, indexed by NameId.
        vector<string> m_Strings;
        /// @brief Number of OnFlush calls.
        u32 m_FlushCount = 0;
        /// @brief Number of OnClose calls.
        u32 m_CloseCount = 0;
    };
}
