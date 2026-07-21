#pragma once

#include <Veng/Veng.h>
#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Diagnostics/TraceSink.h>

#include <span>

// The on-disk trace format's encoder and its shared constants. This is the single writer
// implementation of the normative stream specified in docs/trace-format.md; FileTraceSink drives it
// and the format's tests decode against the spec. Internal to the engine build — the format is a
// public artifact, but the writer is an implementation detail, so this header is not exported.
//
// The stream is a fixed preamble followed by length-prefixed typed sections, with variable-width
// integers throughout. TraceWriter accumulates the whole stream in a byte buffer; a caller commits
// it atomically (FileTraceSink) or inspects it (tests).

namespace Veng::Diagnostics::TraceFileFormat
{
    /// @brief The eight magic bytes at offset 0: ASCII "VENGTRAC".
    inline constexpr u8 Magic[8] = {'V', 'E', 'N', 'G', 'T', 'R', 'A', 'C'};

    /// @brief The format version stored in the preamble and checked on decode.
    inline constexpr u32 FormatVersion = 1;

    /// @brief Size of the fixed preamble in bytes; a reader seeks past it to the first section.
    inline constexpr u32 PreambleSize = 40;

    /// @brief Whether a capture is a triggered stream or a continuous ring dump.
    enum class CaptureMode : u8
    {
        /// @brief A discrete begin/end capture written as string deltas arrive.
        Triggered = 0,
        /// @brief A snapshot of the retained ring, carrying the full string table.
        RingDump = 1,
    };

    /// @brief The build configuration a capture was produced by.
    enum class BuildConfig : u8
    {
        /// @brief An unoptimized debug build.
        Debug = 0,
        /// @brief An optimized release build.
        Release = 1,
    };

    /// @brief The typed sections of the stream, in the order a reader dispatches on.
    enum class SectionType : u32
    {
        /// @brief Capture identity: engine version, executable basename, git provenance.
        Metadata = 1,
        /// @brief Interned strings, as a delta or a full table.
        StringTable = 2,
        /// @brief A track descriptor (thread or virtual).
        Track = 3,
        /// @brief One self-contained chunk of event records.
        Chunk = 4,
        /// @brief Dropped-event and dropped-thread counts.
        Accounting = 5,
        /// @brief Clean-close marker; empty payload, present only on a clean close.
        Trailer = 6,
    };

    /// @brief Whether a track descriptor names a recording thread's lane or a virtual bridge lane.
    enum class TrackKind : u8
    {
        /// @brief A recording thread's own track; its id is a chunk ThreadId.
        Thread = 0,
        /// @brief A virtual track a bridge emits back-dated spans onto; its id is a record TrackId.
        Virtual = 1,
    };

    /// @brief The one-byte tag selecting a counter value's encoding.
    enum class CounterValueTag : u8
    {
        /// @brief A non-negative integer, exact as u64, stored as a varint.
        VarU64 = 0,
        /// @brief An integer, exact as i64, stored as a zigzag varint.
        ZigzagI64 = 1,
        /// @brief Any other f64, stored as eight raw little-endian bytes.
        RawF64 = 2,
    };

    /// @brief Encodes the narrowest counter value tag that round-trips @p value exactly.
    ///
    /// Tries VarU64, then ZigzagI64, then RawF64, and returns the first whose decode reproduces the
    /// original f64 bit-for-bit. The discrimination rule is normative (docs/trace-format.md), so a
    /// decoder reads the tag rather than re-deriving it.
    /// @param value  The counter sample.
    /// @return The tag a writer must emit for @p value.
    [[nodiscard]] CounterValueTag SelectCounterTag(f64 value) noexcept;

    /// @brief One decoded track descriptor, as the writer is handed it.
    struct TrackDescriptor
    {
        /// @brief Whether Id is a thread ThreadId or a virtual TrackId.
        TrackKind Kind = TrackKind::Thread;
        /// @brief The track's id in its kind's space.
        u32 Id = 0;
        /// @brief The kind of work the track carries.
        TrackRole Role = TrackRole::Cpu;
        /// @brief The track's display name; may be empty.
        string Name;
    };

    /// @brief One decoded event, in absolute terms, as the writer re-encodes it compactly.
    struct EventRecord
    {
        /// @brief The record kind (matches TraceFormat::RecordType).
        u8 Type = 0;
        /// @brief The virtual track id, or 0 for the enclosing chunk's thread track.
        u32 Track = 0;
        /// @brief The interned name id (0 = no name).
        NameId Name = 0;
        /// @brief The frame the event measures (absolute).
        u64 Frame = 0;
        /// @brief The span/instant/sample begin, in absolute ticks.
        u64 BeginTicks = 0;
        /// @brief The span end, in absolute ticks; equals BeginTicks for non-scopes.
        u64 EndTicks = 0;
        /// @brief The counter sample value; meaningful only for a Counter record.
        f64 Value = 0.0;
    };

    /// @brief One decoded chunk: its self-contained framing plus its records.
    struct ChunkData
    {
        /// @brief The producing thread track.
        ThreadId Thread = 0;
        /// @brief Monotonic sequence number within the thread's chunk stream.
        u64 SequenceNumber = 0;
        /// @brief Absolute tick base every record BeginTicks is measured from.
        u64 TimestampBase = 0;
        /// @brief The records, in emission order.
        vector<EventRecord> Records;
    };

    /// @brief Streaming encoder for the on-disk trace format.
    ///
    /// Sections are appended in call order after the preamble; the caller commits the accumulated
    /// bytes (atomically, off the recording thread) once the trailer is written. The writer holds no
    /// I/O — it only builds the byte stream — so it is trivially testable and reusable by the fixture
    /// generator.
    class TraceWriter
    {
    public:
        /// @brief Begins a stream, writing the fixed preamble.
        /// @param mode           Triggered or ring dump.
        /// @param config         The build configuration that produced the capture.
        /// @param profileEnabled Whether the producer was built with VE_PROFILE.
        /// @param tickFrequency  Ticks per second of the trace clock.
        /// @param tickBase       A reference tick near capture start.
        TraceWriter(CaptureMode mode, BuildConfig config, bool profileEnabled, u64 tickFrequency,
                    u64 tickBase);

        /// @brief One git-provenance entry: a tree's name, short SHA, and dirty flag.
        struct Provenance
        {
            /// @brief The tree's name (e.g. "engine").
            string Name;
            /// @brief The git short SHA at capture time.
            string ShortSha;
            /// @brief Whether the tree had uncommitted changes at capture time.
            bool Dirty = false;
        };

        /// @brief Writes the Metadata section — only the enumerated identity fields.
        /// @param engineVersion       The engine version string.
        /// @param executableBasename  The executable's basename (never its path).
        /// @param provenance          One entry per submodule involved.
        void WriteMetadata(string_view engineVersion, string_view executableBasename,
                           std::span<const Provenance> provenance);

        /// @brief Writes a StringTable section.
        /// @param firstId  Id of the first string; ids run firstId..firstId+strings.size()-1.
        /// @param strings  The strings in ascending id order.
        /// @param isFull   True for a full table (a ring dump), false for a delta.
        void WriteStringTable(NameId firstId, std::span<const string_view> strings, bool isFull);

        /// @brief Writes a Track descriptor section.
        /// @param track  The descriptor.
        void WriteTrack(const TrackDescriptor& track);

        /// @brief Writes a Chunk section, re-encoding its records in the compact form.
        /// @param chunk  The decoded chunk.
        void WriteChunk(const ChunkData& chunk);

        /// @brief Writes the Accounting section.
        /// @param droppedEvents   Events discarded on ring wraps.
        /// @param droppedThreads  Thread registrations refused for exceeding the max.
        void WriteAccounting(u64 droppedEvents, u64 droppedThreads);

        /// @brief Writes the Trailer section, marking a clean close. Call last.
        void WriteTrailer();

        /// @brief Returns the accumulated stream bytes.
        [[nodiscard]] const vector<u8>& GetBytes() const { return m_Bytes; }

        /// @brief Moves the accumulated stream bytes out.
        [[nodiscard]] vector<u8> TakeBytes() { return std::move(m_Bytes); }

    private:
        /// @brief Appends a raw byte.
        void PutU8(u8 value);
        /// @brief Appends a little-endian u16.
        void PutU16(u16 value);
        /// @brief Appends a little-endian u32.
        void PutU32(u32 value);
        /// @brief Appends a little-endian u64.
        void PutU64(u64 value);
        /// @brief Appends an unsigned LEB128 varint.
        void PutVarint(u64 value);
        /// @brief Appends a zigzag-encoded signed varint.
        void PutZigzag(i64 value);
        /// @brief Appends a varint length-prefixed UTF-8 string.
        void PutString(string_view text);
        /// @brief Opens a section: appends its type and reserves its length prefix.
        /// @return The byte offset of the reserved length prefix, for EndSection.
        [[nodiscard]] usize BeginSection(SectionType type);
        /// @brief Closes a section: back-patches the payload length reserved by BeginSection.
        /// @param lengthOffset  The offset BeginSection returned.
        void EndSection(usize lengthOffset);

        /// @brief The accumulated stream.
        vector<u8> m_Bytes;
    };

    /// @brief Appends an unsigned LEB128 varint to a byte buffer. Shared with decoders.
    /// @param out    The buffer to append to.
    /// @param value  The value to encode.
    void AppendVarint(vector<u8>& out, u64 value);

    /// @brief Reads an unsigned LEB128 varint from a byte span.
    /// @param data    The bytes.
    /// @param offset  In/out cursor; advanced past the varint. Unchanged on failure.
    /// @param out     Receives the decoded value.
    /// @return True on success; false if the bytes end mid-varint.
    [[nodiscard]] bool ReadVarint(std::span<const u8> data, usize& offset, u64& out);

    /// @brief Decodes a zigzag-encoded signed integer.
    /// @param encoded  The zigzag-encoded unsigned value.
    /// @return The signed value.
    [[nodiscard]] i64 DecodeZigzag(u64 encoded);
}
