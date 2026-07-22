#pragma once

#include <Veng/Veng.h>

#include <span>

// A standalone decoder for the veng trace binary format, written against the normative
// specification in docs/trace-format.md — not against the engine's writer. It shares no code with
// FileTraceSink's encoder: the two are independent implementations of one document, so a decoder
// that only works because it mirrors a writer quirk would not be a second implementation.
//
// The decoder reads a whole capture into an in-memory model (VengTrace::DecodedTrace); the
// ChromeTraceConverter projects that model onto Chrome Trace Event JSON. A truncated capture is a
// first-class result, not an error: the decoder keeps every section it fully parsed and reports the
// stream as truncated.

namespace Veng::VengTrace
{
    /// @brief The kind of event a record carries; matches the format's record-type field (bits 0-1).
    enum class RecordType : u8
    {
        /// @brief A completed scope with a begin tick and a duration.
        ScopeComplete = 0,
        /// @brief A sampled numeric counter value.
        Counter = 1,
        /// @brief A zero-duration marked instant.
        Instant = 2,
    };

    /// @brief Whether a track descriptor names a recording thread's lane or a virtual bridge lane.
    enum class TrackKind : u8
    {
        /// @brief A recording thread's own track; its id is a chunk ThreadId.
        Thread = 0,
        /// @brief A virtual track a bridge emits back-dated spans onto; its id is a record TrackId.
        Virtual = 1,
    };

    /// @brief The kind of work a track carries, so a viewer can group and place it.
    enum class TrackRole : u8
    {
        /// @brief A CPU thread's own spans.
        Cpu = 0,
        /// @brief GPU work, back-dated onto a virtual track.
        Gpu = 1,
        /// @brief Anything a producer wanted a separate lane for.
        Custom = 2,
    };

    /// @brief Whether a capture was a triggered stream or a continuous ring dump.
    enum class CaptureMode : u8
    {
        /// @brief A discrete begin/end capture.
        Triggered = 0,
        /// @brief A snapshot of the retained ring.
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

    /// @brief The one-byte tag selecting a counter value's on-disk encoding.
    enum class CounterValueTag : u8
    {
        /// @brief A non-negative integer, exact as u64, stored as a varint.
        VarU64 = 0,
        /// @brief An integer, exact as i64, stored as a zigzag varint.
        ZigzagI64 = 1,
        /// @brief Any other f64, stored as eight raw little-endian bytes.
        RawF64 = 2,
    };

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

    /// @brief The capture-identity fields carried by the Metadata section.
    struct Metadata
    {
        /// @brief The engine version string.
        string EngineVersion;
        /// @brief The executable's basename (never its path).
        string ExecutableBasename;
        /// @brief One provenance entry per submodule involved.
        vector<Provenance> Submodules;
    };

    /// @brief One decoded track descriptor.
    struct Track
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

    /// @brief One decoded event, in absolute terms.
    struct Event
    {
        /// @brief The record kind.
        RecordType Type = RecordType::ScopeComplete;
        /// @brief The producing thread's track id (the enclosing chunk's ThreadId).
        u32 Thread = 0;
        /// @brief The virtual track id if this record overrode its track, else 0.
        u32 VirtualTrack = 0;
        /// @brief Whether the record carried a virtual-track override.
        bool HasVirtualTrack = false;
        /// @brief The interned name id (0 = no name).
        u32 Name = 0;
        /// @brief The frame this event measures (absolute).
        u64 Frame = 0;
        /// @brief The span/instant/sample begin, in absolute ticks.
        u64 BeginTicks = 0;
        /// @brief The span end, in absolute ticks; equals BeginTicks for a non-scope.
        u64 EndTicks = 0;
        /// @brief The counter sample value; meaningful only for a Counter record.
        f64 Value = 0.0;
        /// @brief The counter value's on-disk tag; meaningful only for a Counter record.
        CounterValueTag ValueTag = CounterValueTag::VarU64;
    };

    /// @brief The whole capture, decoded into an in-memory model.
    struct DecodedTrace
    {
        /// @brief The format version read from the preamble.
        u32 FormatVersion = 0;
        /// @brief Ticks per second of the trace clock.
        u64 TickFrequency = 0;
        /// @brief The preamble's reference tick anchor.
        u64 TickBase = 0;
        /// @brief The capture mode.
        CaptureMode Mode = CaptureMode::Triggered;
        /// @brief The build configuration that produced the capture.
        BuildConfig Config = BuildConfig::Debug;
        /// @brief Whether the producer was built with VE_PROFILE.
        bool ProfileEnabled = false;

        /// @brief The capture identity, if a Metadata section was present.
        optional<Metadata> Meta;
        /// @brief The track descriptors, in encounter order.
        vector<Track> Tracks;
        /// @brief Every event, flattened across chunks in decode order.
        vector<Event> Events;
        /// @brief The interned string table, id 1.. at index 0.. (id 0 = no name).
        vector<string> Strings;

        /// @brief Events discarded on ring wraps, if an Accounting section was present.
        u64 DroppedEvents = 0;
        /// @brief Thread registrations refused, if an Accounting section was present.
        u64 DroppedThreads = 0;
        /// @brief Whether an Accounting section was present.
        bool HasAccounting = false;

        /// @brief Type values of sections the decoder did not recognize and skipped.
        vector<u32> UnknownSections;

        /// @brief A Trailer was reached: the capture is complete.
        bool Complete = false;
        /// @brief The stream ended before a Trailer: the capture is truncated.
        bool Truncated = false;

        /// @brief Resolves an interned name id to its string (empty for id 0 or an unknown id).
        /// @param id  The interned name id.
        /// @return The string, or an empty view.
        [[nodiscard]] string_view Resolve(u32 id) const;
    };

    /// @brief The outcome of a decode attempt that could not even begin.
    enum class DecodeStatus : u8
    {
        /// @brief The stream decoded (possibly as a truncated capture — still a success).
        Ok = 0,
        /// @brief The input is too small or lacks the magic — not a veng capture.
        NotACapture = 1,
        /// @brief The format major version is one this decoder does not recognize.
        UnknownVersion = 2,
    };

    /// @brief The result of decoding a byte stream.
    struct DecodeResult
    {
        /// @brief Whether decoding began, and if not, why.
        DecodeStatus Status = DecodeStatus::Ok;
        /// @brief The decoded capture; meaningful only when Status is Ok.
        DecodedTrace Trace;
        /// @brief The format version read, even when it was rejected (0 if none was read).
        u32 FormatVersion = 0;
    };

    /// @brief Decodes a whole capture from its bytes, tolerating truncation per the specification.
    ///
    /// Reads the preamble, then sections until a Trailer (clean) or until a section header or
    /// payload cannot be fully read (truncated), keeping every fully-parsed section. An unrecognized
    /// major version is rejected cleanly rather than misparsed.
    /// @param data  The capture bytes.
    /// @return The decode result.
    [[nodiscard]] DecodeResult Decode(std::span<const u8> data);
}
