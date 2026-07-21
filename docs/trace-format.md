# The veng trace format

This is the **normative** specification of the on-disk shape of a profiler capture — the compact
binary stream `Veng::Diagnostics::FileTraceSink` produces and every decoder reads. Two independent
decoders are written against *this document*, not against each other or against the writer: the
`vengtrace` converter and the observatory's ingest. Where a decoder and the writer disagree, the
disagreement is resolved against this specification.

The format is a **self-describing, versioned, streamable** byte stream. Streamable means a writer
appends without seeking and a reader that reaches the end of a truncated file still recovers every
complete section before the cut. Compactness comes from the *encoding* (variable-width integers,
per-chunk timestamp bases, narrowest-form counter values), not from a compression pass; the section
framing leaves room to wrap a section in a codec later without a format change.

## Conventions

- **Byte order** is little-endian for every fixed-width integer.
- **`varint`** is unsigned LEB128: 7 payload bits per byte, low bits first, the high bit set on every
  byte but the last. It encodes lengths, counts, ids, timestamp deltas, and unsigned counter values.
- **`zigzag varint`** encodes a signed integer as `varint((n << 1) ^ (n >> 63))` — small-magnitude
  values (of either sign) stay short. It encodes frame deltas and signed counter values.
- **`string`** is a `varint` byte length followed by that many UTF-8 bytes. The empty string is a
  single `0x00` byte.
- **Ticks** are in the trace clock's raw domain (`Veng::Diagnostics::NowTicks()`): a monotonic
  counter whose frequency is recorded in the preamble and whose epoch is arbitrary, so only tick
  *differences* carry meaning. A decoder converts to nanoseconds with `TraceTicksToNanos` using the
  recorded frequency; it must not assume nanoseconds or a fixed period. On Apple Silicon the counter
  is the 24 MHz architected timer (~41.7 ns/tick); on other platforms it is `steady_clock` in its
  native period. The format carries the frequency precisely so the same bytes decode identically on
  any host.

## File layout

```
+-----------+
| preamble  |   fixed 40 bytes
+-----------+
| section   |   type + length-prefixed payload
| section   |
|   ...      |
| trailer   |   present iff the capture closed cleanly
+-----------+
```

## Preamble (fixed, 40 bytes, offset 0)

| field            | type      | notes                                                       |
|------------------|-----------|-------------------------------------------------------------|
| `Magic`          | `u8[8]`   | ASCII `VENGTRAC` (`56 45 4E 47 54 52 41 43`)                |
| `FormatVersion`  | `u32`     | `1`. See [Versioning](#versioning-and-compatibility).       |
| `PreambleSize`   | `u32`     | `40`. A reader seeks to the first section at this offset.   |
| `TickFrequency`  | `u64`     | Ticks per second of the trace clock (`TraceTickFrequency`). |
| `TickBase`       | `u64`     | A reference tick near capture start; informational anchor.  |
| `CaptureMode`    | `u8`      | `0` triggered, `1` ring dump.                               |
| `BuildConfig`    | `u8`      | `0` Debug, `1` Release.                                     |
| `ProfileEnabled` | `u8`      | `1` when built with `VE_PROFILE`, else `0`.                 |
| `Reserved`       | `u8[5]`   | Zero. Ignored by readers.                                   |

The clock's resolution is `1 / TickFrequency` seconds; its epoch is boot-relative and carries no
wall-clock alignment. `TickBase` is a convenience anchor only — every chunk carries its own
**absolute** timestamp base (below), so no record depends on `TickBase`.

A reader honours `PreambleSize`: it reads the fields it knows and advances to `PreambleSize` before
the first section, so a future preamble that grows stays readable by this version's skip logic.

## Section framing

After the preamble the file is a flat sequence of sections. Each section is:

| field          | type   | notes                                          |
|----------------|--------|------------------------------------------------|
| `Type`         | `u32`  | Section type (below).                          |
| `PayloadBytes` | `u32`  | Length of `Payload` in bytes.                  |
| `Payload`      | bytes  | `PayloadBytes` bytes, format per `Type`.       |

A reader that does not recognize `Type` reads `PayloadBytes` and skips the payload — this is what
lets a new section type be added without a version bump. A reader **must** frame every section this
way (never by parsing the payload to find its end), so an unknown section is always skippable.

Section types:

| value | name           | purpose                                             |
|-------|----------------|-----------------------------------------------------|
| `1`   | `Metadata`     | Capture identity — engine build, git provenance.    |
| `2`   | `StringTable`  | Interned strings, as a delta or a full table.       |
| `3`   | `Track`        | A track descriptor (thread or virtual).             |
| `4`   | `Chunk`        | One self-contained chunk of event records.          |
| `5`   | `Accounting`   | Dropped-event and dropped-thread counts.            |
| `6`   | `Trailer`      | Clean-close marker; empty payload.                  |

Section order is: `Metadata` first, then `StringTable` / `Track` / `Chunk` / `Accounting` sections
interleaved in the order they were produced, then a single `Trailer` last on a clean close. A
decoder does not rely on the interleaving of the middle sections beyond the string-table rule below;
it builds its track and string maps as sections arrive and resolves records against the accumulated
maps.

### Metadata section (type 1)

Records **only** the enumerated identity fields — nothing derived from the host environment. The
absolute path of the executable, environment variables, the command line, and the user or host name
are **excluded by construction**: they are never written, because a capture is a shareable artifact
(a committed fixture lives in a public repository) and an identity field is the likeliest place for
a private path to leak.

| field                | type            | notes                                             |
|----------------------|-----------------|---------------------------------------------------|
| `EngineVersion`      | `string`        | The engine version string.                        |
| `ExecutableBasename` | `string`        | The executable's basename only — never its path.  |
| `SubmoduleCount`     | `u16`           | Number of provenance entries below.               |
| per submodule:       |                 | `SubmoduleCount` repetitions of:                  |
| &nbsp;&nbsp;`Name`   | `string`        | The tree's name (e.g. `engine`).                  |
| &nbsp;&nbsp;`ShortSha`| `string`       | The git short SHA at capture time.                |
| &nbsp;&nbsp;`Dirty`  | `u8`            | `1` if that tree had uncommitted changes, else `0`. |

Git provenance is recorded here and nowhere else: a downstream ingest attributes a capture to a
commit purely from these fields. A capture produced by a consumer of the engine carries one entry
per tree involved — the engine's HEAD and the consumer's — each with its own dirty flag.

### StringTable section (type 2)

Interned names, resolved by id from every event record. Id `0` is reserved for "no name" and is
never present in the table.

| field       | type       | notes                                                        |
|-------------|------------|--------------------------------------------------------------|
| `IsFull`    | `u8`       | `1` = full table replacing any prior; `0` = delta appended.  |
| `FirstId`   | `varint`   | Id of the first string; ids run `FirstId .. FirstId+Count-1`.|
| `Count`     | `varint`   | Number of strings.                                           |
| `Strings`   | `string`×`Count` | The strings, in ascending id order from `FirstId`.     |

A section is either a **delta** (`IsFull = 0`), which the decoder appends to its table at the given
ids, or a **full table** (`IsFull = 1`, `FirstId = 1`, `Count` = every interned id), which replaces
the decoder's table. The `IsFull` flag is authoritative — a decoder applies each section by its
flag. A producer that streams incrementally emits deltas as ids are interned; a producer that builds
the whole capture and commits it at once — as the file sink does — emits a single full table.

**A ring dump must emit a full table.** This is required, not optional: interned ids accumulate for
the life of the capture, so a ring dump whose early chunks were discarded still references ids that
were interned in those discarded chunks. Emitting only the strings interned since the last dump would
leave those ids unresolvable, and a decoder that silently dropped unresolvable ids would produce a
capture that merely looks sparse. A decoder resolving a record's name id it has no string for treats
it as a malformed capture, not as an empty name.

### Track section (type 3)

One descriptor per track. A **thread track** is a recording thread's own lane; a **virtual track**
is a lane a bridge emits back-dated spans onto (the GPU is the first tenant). The two id spaces are
distinct, so the descriptor names which space its id is in.

| field   | type       | notes                                                          |
|---------|------------|----------------------------------------------------------------|
| `Kind`  | `u8`       | `0` thread track, `1` virtual track.                           |
| `Id`    | `varint`   | Thread-track: the chunk `ThreadId`. Virtual: the record's `TrackId`. |
| `Role`  | `u8`       | `0` CPU, `1` GPU, `2` custom.                                  |
| `Name`  | `string`   | Display name; may be empty (a decoder falls back to the id).   |

A record resolves to a track as follows: a record with no track override belongs to the **thread
track** whose `Id` equals the enclosing chunk's `ThreadId`; a record with a track override belongs
to the **virtual track** whose `Id` equals the override. GPU passes are a virtual track like any
other, which is what lets a viewer lay CPU and GPU spans on one timeline.

### Chunk section (type 4)

A self-contained run of event records from one thread. **Every chunk is independently decodable**
with no state carried from the chunk before it except the string table: it carries its own absolute
timestamp base, its own base frame, and a monotonically increasing sequence number. This is
normative — the recording buffers are rings of whole chunks, so a chunk is the smallest unit that
survives a ring wrap intact.

| field           | type       | notes                                                       |
|-----------------|------------|-------------------------------------------------------------|
| `ThreadId`      | `varint`   | The producing thread track.                                 |
| `SequenceNumber`| `varint`   | Monotonic within the thread's chunk stream.                 |
| `TimestampBase` | `u64`      | **Absolute** ticks; every record `BeginDelta` is relative to this. |
| `BaseFrame`     | `varint`   | Frame index the per-record frame deltas are relative to.    |
| `RecordCount`   | `varint`   | Number of records.                                          |
| `Records`       | record×`RecordCount` | Encoded as below.                                 |

**Sequence numbers and gaps.** A capture's first chunk on a given thread need **not** be sequence 0.
A ring dump normally begins mid-sequence, because wrapping discards whole chunks; the sequence
numbers are what make the discarded span visible as a *gap* rather than as silence. A decoder must
accept a first chunk whose sequence number is not 0 and must treat a jump in sequence numbers as
dropped chunks, not as an error. The consequence, stated plainly: the ring's configured duration is
honoured only to within one chunk.

Each **record**:

| field          | type            | notes                                                    |
|----------------|-----------------|----------------------------------------------------------|
| `Tag`          | `u8`            | bits 0–1 = record type; bit 2 = track override present.  |
| `TrackId`      | `varint`        | Present iff `Tag` bit 2 is set; the virtual track id.    |
| `FrameDelta`   | `zigzag varint` | `record frame − BaseFrame`. See the frame contract.      |
| `BeginDelta`   | `varint`        | `begin tick − TimestampBase`.                            |
| type payload   | —               | Per record type below.                                   |

Record types (`Tag` bits 0–1):

| value | type            | payload                                                          |
|-------|-----------------|-----------------------------------------------------------------|
| `0`   | `ScopeComplete` | `Name` (`varint`), `Duration` (`varint`, ticks, `end − begin`). |
| `1`   | `Counter`       | `Name` (`varint`), then a [counter value](#counter-values).     |
| `2`   | `Instant`       | `Name` (`varint`).                                              |

`Name` is an interned string id (`0` = no name). `Duration` is stored rather than an end tick because
it is small and non-negative in the common case, so its varint is a byte or two.

#### Counter values

`VE_PROFILE_COUNTER` takes an `f64` and the format stores it **losslessly**, but a raw `f64` is eight
bytes — the largest field in any record — and most counters are small integers (queue depth, draw
calls, bytes loaded). So a counter value is a one-byte **type tag** selecting the encoding, and the
writer picks the narrowest form that round-trips the value **exactly**:

| tag | encoding          | bytes             | chosen when                                    |
|-----|-------------------|-------------------|------------------------------------------------|
| `0` | `varint` u64      | 1–10              | value is a non-negative integer, exact as `u64`.|
| `1` | `zigzag varint` i64 | 1–10            | value is an integer, exact as `i64`, and tag 0 does not apply. |
| `2` | raw `f64`         | 8 (little-endian) | otherwise (fractional, out of integer range, NaN, ±inf, −0). |

The discrimination rule is normative and belongs to the writer: it tries tag 0, then tag 1, then tag
2, and emits the first whose decode reproduces the original `f64` bit-for-bit. A decoder reads the
tag and decodes accordingly; it does not guess. Both decoders therefore read the same bytes the same
way rather than each inventing a heuristic.

### The frame index contract

Every event names the frame it **measures**, never the frame that was current when it was recorded.
For a CPU scope the two coincide and `FrameDelta` is typically `0`. The field exists for the GPU
bridge: `Context::GetLastGpuPassTimings()` reports a frame that retired several frames ago, so a
writer that stamped a GPU duration with the *current* frame index would misattribute every GPU cost
by a fixed offset — a bug that looks like plausible data. A writer emitting a back-dated span (a GPU
pass) **must** stamp it with the frame it measured; that frame is earlier than `BaseFrame`, so its
`FrameDelta` is negative, which the zigzag encoding carries. A writer emitting GPU durations against
the current frame index is wrong per this specification.

### Accounting section (type 5)

Counts that make a lossy capture visibly lossy rather than quietly short.

| field           | type       | notes                                                    |
|-----------------|------------|----------------------------------------------------------|
| `DroppedEvents` | `varint`   | Events discarded (a ring wrap onto un-drained records).  |
| `DroppedThreads`| `varint`   | Thread registrations refused for exceeding the max.      |

### Trailer section (type 6)

An empty-payload section written **only** on a clean close. Its presence means the capture is
complete; its absence means the capture was cut short. A reader that reaches a `Trailer` stops and
reports the capture complete.

## Truncation

A capture is truncated when the writer was killed mid-stream — no `Trailer` is present, and the last
section may itself be incomplete. A reader **must** handle this: it reads sections until either a
`Trailer` (clean) or it cannot read a full section header (8 bytes) or a section's full payload
(`PayloadBytes` bytes) from the remaining input (truncated). On truncation it keeps every section it
fully parsed, discards the torn final section, and reports the capture as truncated. It never fails
outright on a short file — a truncated capture reads up to its last complete section. Combined with
the `Accounting` counts, a short capture is always *visibly* short.

## Versioning and compatibility

`FormatVersion` is the one version number the format checks. A decoder that does not recognize the
major version **rejects the file cleanly** rather than misparsing it — the event record layout is
tied to the version, and reading version *N+1* records as version *N* would produce plausible-looking
garbage.

Within a recognized version:

- **New section types may be added and are ignorable.** A reader skips a section whose `Type` it does
  not know (via `PayloadBytes`), so a later writer can add a section this reader will not choke on.
- **The event record layout may not change without a version bump.** The record encoding, the chunk
  framing, the counter value tags, and the preamble's fixed fields are fixed for a version; a change
  to any of them is a new `FormatVersion`.

This file format's version is independent of the network `ProtocolVersion` and of the `.vengpack`
archive version; the three are unrelated and move independently.
