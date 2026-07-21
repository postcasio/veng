#pragma once

#include <Veng/Veng.h>
#include <Veng/Diagnostics/TraceSink.h>

// The chunk and record layout the diagnostics core writes into per-thread buffers
// and hands to a TraceSink. This is an internal, fixed-width provisional encoding:
// enough for the in-memory test sink to round-trip events, and structured as the
// design requires (self-contained chunks with their own timestamp base, first-record
// offset, and sequence number). The normative on-disk stream is a separate module;
// nothing outside Diagnostics/ and its tests decodes this.

namespace Veng::Diagnostics::TraceFormat
{
    /// @brief The kind of event a record carries.
    enum class RecordType : u8
    {
        /// @brief A completed scope with begin and end timestamps.
        ScopeComplete = 0,
        /// @brief A sampled numeric counter value.
        Counter = 1,
        /// @brief A zero-duration marked instant.
        Instant = 2,
    };

    /// @brief Self-contained header at the start of every chunk's bytes.
    ///
    /// A chunk decodes from its own base with no reference to any other chunk: the
    /// timestamp base anchors every record's deltas, FirstRecordOffset locates the
    /// first record, and SequenceNumber orders chunks from the same thread.
    struct ChunkHeader
    {
        /// @brief Absolute trace-clock ticks (NowTicks) every record delta in this chunk is relative to.
        u64 TimestampBase = 0;
        /// @brief Monotonic sequence number within the producing thread's chunk stream.
        u64 SequenceNumber = 0;
        /// @brief Byte offset of the first record; equals sizeof(ChunkHeader).
        u32 FirstRecordOffset = 0;
        /// @brief Total record bytes after the header; written when the chunk is sealed.
        u32 RecordBytes = 0;
    };
    static_assert(sizeof(ChunkHeader) == 24,
                  "ChunkHeader layout is part of the provisional encoding");

    /// @brief One fixed-width event record.
    ///
    /// Timestamps are trace-clock tick deltas from the chunk's TimestampBase. For a
    /// scope, Begin and EndOrValue are the endpoints; for a counter, EndOrValue holds
    /// the f64 value's bits and Begin the sample time; for an instant, Begin is the
    /// point and EndOrValue is 0.
    struct EventRecord
    {
        /// @brief The record's RecordType.
        u8 Type = 0;
        /// @brief Reserved for alignment.
        u8 Reserved0 = 0;
        /// @brief Reserved for alignment.
        u16 Reserved1 = 0;
        /// @brief Track the record belongs to; 0 means the producing thread's own track.
        u32 Track = 0;
        /// @brief Interned name id.
        u32 Name = 0;
        /// @brief Frame index in effect when the record was written.
        u32 Frame = 0;
        /// @brief Begin timestamp as a delta from TimestampBase, in trace-clock ticks.
        u64 BeginDelta = 0;
        /// @brief Scope end delta, counter value bits, or 0 for an instant.
        u64 EndOrValue = 0;
    };
    static_assert(sizeof(EventRecord) == 32,
                  "EventRecord layout is part of the provisional encoding");

    /// @brief Bytes occupied by one record.
    inline constexpr u32 RecordStride = sizeof(EventRecord);
}
