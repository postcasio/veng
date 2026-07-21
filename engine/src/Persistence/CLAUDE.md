# Persistence — the durable-state subsystem

`Veng/Persistence/` is where state outlives worlds, sessions, and the process. The namespace is
flat `Veng::` — the directory carries the grouping, not a nested namespace.

## The store

`Veng/Persistence/Store.h` is the substrate: **one `Store` per slot directory per process**,
opened with `Store::Open(slotDirectory)`. Everything below is that one class.

### Families, keys, records

State is partitioned into **families** (`StoreFamily`), each a keyspace named by a minted
**`StoreFamilyId`** (`vengc generate-family-id`) and persisted as its own file in the slot. A
family registration carries its id, a **file stem**, a format **version**, and four optional hooks
(`Capture`, `RehydrateKeys`, `Rehydrate`, `Migrate`). Ids come from a single flat space — the
engine's own families are minted from the same space consumers mint from, with no reserved numeric
range — and a double registration is a fatal assert.

A record keys on an opaque **`StoreKey { u64 Lo, Hi }`**, taken verbatim: the store never
interprets the bits, so any 128-bit id space a consumer already owns keys records directly. A
**`StoreRecord`** is `{ i64 CapturedAtWall; vector<ComponentBlob> }`, and a **`ComponentBlob`** is
`{ TypeId; bytes }` — the `WriteFields` output of one reflected component, or an opaque blob under
`InvalidTypeId`.

Reads and writes (`Read`/`Write`/`Erase`/`EraseAll`/`ForEachRecord`) are memory-only against
per-family tables loaded at open; `Flush` is the only file I/O. `Subscribe` is the pub seam an
event-driven projection hangs off: it fires per effective mutation, after the table reflects the
change, may re-enter, and is never removed.

**`CaptureScene(Scene&)`** runs every capture-registered family over a scene, stamping each record
with the current wall clock; **`RehydrateScene(Scene&)`** applies stored records over a freshly
built scene, handing each `Rehydrate` hook the wall seconds elapsed since capture, clamped >= 0. A
wall clock regresses (NTP, suspend, a manual change), so the clamp is at the seam rather than left
to each hook. The elapsed argument is a **policy stub**: an identity implementation restores state
verbatim and ignores it, and nothing in the engine derives anything from it.

### The flush protocol

`Flush` persists the dirty families atomically for the whole slot:

1. Each dirty family writes `<stem>.<nextGeneration>.vst`, fsynced. The committed record still
   names only the prior generation, so a crash here leaves the prior generation whole.
2. The **commit record** — every family's committed file, dirty ones at the new generation and
   clean ones keeping their old file — is written to `slot.commit.<nextGeneration>.tmp`, fsynced,
   and **renamed** onto `slot.commit`. That rename is the whole slot's commit point.
3. The directory is synced (POSIX). Windows has no directory-handle fsync, so the rename's
   durability rides the synced write; that asymmetry is real and is not compensated for.
4. Superseded family files are deleted best-effort; `Open`'s sweep collects any straggler.

The rename goes through `std::filesystem::rename`, not `::rename`: the narrow path conversion a C
rename needs is lossy on Windows, which would fail every flush for a slot under a non-ASCII user
profile while the rest of the write path worked.

On-disk vocabulary: family files carry the magic `VNG.VST1`, the commit record `VNG.CMT1`, and
`slot.lock` holds the exclusive advisory lock (flock / a no-share Win32 open). Contention fails the
open loudly with the reason; the OS releases the lock on any process exit, so a crash leaves no
stale lock.

### Versions and migration

A family's **version lives in the file header only**, never per record. Schema drift *within* a
version is absorbed by the reflection walker's tolerant read (an unknown field skipped, a missing
field defaulted). A version *bump* is the explicit `Migrate` hook, run lazily at `Read` and swept
over the remainder at `Flush`/`ForEachRecord` so a written file carries one version throughout. An
older file with no `Migrate` hook reads as no records, logged once.

Records of families **never registered in this process are preserved verbatim** across a flush, so
a tool or a partially-configured process cannot silently drop a slot's other families.

### Untrusted input

A slot directory is something a consumer may point anywhere, and a family file is input the store
did not necessarily write, so four rules hold at the boundary:

- **File stems are validated, never trusted as path components.** A stem is a single path component
  of `[A-Za-z0-9._-]`, non-empty, at most `Store::MaxFileStemLength` bytes, and neither `.` nor
  `..` (`Store::IsValidFileStem`). Stems arrive from the commit record and are interpolated into a
  path on both load and write — unregistered families are preserved across flush — so an illegal
  stem fails the open rather than reading or writing outside the slot.
- **Stems are unique, not just ids.** Two families sharing a stem write the same file. A duplicate
  is a fatal assert at registration and a refused open when it comes off disk.
- **Counts are bounded before they are trusted.** Record, component, and family counts read from a
  file are checked against the bytes actually remaining before they drive an allocation.
- **The sweep is scoped, and a foreign slot is refused.** `Open` deletes only *its own*
  unreferenced files (`<stem>.<digits>.vst` and the commit temporaries); everything else in the
  directory is left alone, so opening a slot never means emptying a directory. The store owns the
  `slot.` file-name prefix: an unrecognized file under that prefix, or a `slot.commit` it cannot
  read, fails the open naming what it found rather than presenting the directory as a fresh empty
  slot.

### Consuming it

Family ids are minted with **`vengc generate-family-id`**, the sibling of `generate-type-id` /
`generate-asset-type`, and written as zero-padded 16-digit hex (`0x…ULL`). Engine-owned file stems
are namespaced with a `veng.` prefix so an engine family can never collide with the obvious
consumer choice for the same concept.

The store resolves no directory of its own: the slot directory is supplied by the caller.
`Platform/UserPaths.h` is the natural provider of a per-user root.

## Save slots

## Store patterns

## The session-store binding

`Veng/Persistence/SessionStore.h` is the store backing the engine ships for `SessionRegistry`'s
durability hooks. The registry keeps its per-account records for the life of the process unless
`LoadSession`/`SaveSession` are set; the engine owns *when* durability fires (first admit,
disconnect, teardown, the debounced checkpoint) and *what* is encoded, and the hook pair owns only
*where* the bytes land. This is the engine's answer to "where": one family, one record per account.

- **`SessionsFamily`** — the engine's minted family id, drawn from the same flat space consumers
  mint from (there is no reserved numeric range). Its file stem is **`veng.sessions`**: a bare
  `sessions` is the stem a consumer is most likely to pick for the same concept, and two families
  sharing a stem is a refused registration, so engine stems carry the `veng.` prefix.
- **`RegisterSessionFamily(Store&)`** registers it, once per opened store. It is separate from the
  hooks rather than a side effect of building them, and idempotent (it asks `IsFamilyRegistered`
  first): a consumer that binds more than one session-hosting struct against a single store would
  otherwise register the same family twice, which is fatal.
- **`MakeSessionHooks(function<Store*()>, SessionStoreInfo)`** returns the `LoadSession`/
  `SaveSession` pair to assign onto whichever struct the consumer fills — `SessionRegistryInfo`, or
  the mirrored fields on `ApplicationInfo` and `Net::ServerHostInfo`.

**A source, not a `Store&`.** A registry is built once, at init, and its hooks are frozen from that
moment; a store is scoped to an open slot, since it holds the slot's exclusive lock and must be
dropped before another slot opens. A captured reference would name an object that does not exist yet
at init and a destroyed one afterwards. The source resolves per call instead, which also makes "no
store right now" expressible: **a source returning null is the memory-only posture** — loads report
no record, saves are dropped — rather than a special case bolted beside the binding. Setting no
hooks at all is unchanged and still the zero-config default.

**The record.** Keyed on the **full 128-bit account id** (`StoreKey{ .Lo = account.Lo,
.Hi = account.Hi }`) — both halves are load-bearing, and keying on the low word alone would collapse
two accounts onto one record. The value is the registry's encoded blob stored verbatim as a single
`ComponentBlob` tagged `TypeIdOf<Net::SessionRecord>()`: the store does not re-model the record (its
encoding is already the tolerant reflection walker), but the tag is the only thing identifying those
bytes to a debug dump, a save inspector, or a later migration.

**`SessionStoreInfo::FlushOnSave`** defaults true, because a session save is a genuine durability
point — a disconnect may precede process death. The cost is that `Store::Flush` is whole-slot: every
dirty family is rewritten and synced, so on a large slot an unrelated family pays for someone's
disconnect. A consumer checkpointing on its own cadence sets it false. `SaveSession` returns `void`,
so a failed flush can only be logged; a consumer that must observe write failures flushes itself.

The raw hook pair remains the extension seam — a consumer with bespoke storage skips this header
entirely.

## The local account store
