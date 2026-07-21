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

`Veng/Persistence/SaveSlots.h` is the thin layer between "a root directory" and "an open `Store`":
name rules, slot resolution and enumeration, and the open/create entry point. It exists so every
consumer does not re-write the same directory arithmetic, and so the engine owns a slot vocabulary
its own tooling can rely on.

**The root is always supplied by the caller** — these helpers resolve nothing global, exactly as
`Store::Open` resolves no directory of its own. `Platform/UserPaths.h`'s `UserDataDir(application)`
is the natural provider of a per-user root and is the helpers' first documented consumer path, but
it is **documented, not mandated**: a portable application may root beside its executable and a test
roots in a temporary directory. Any intermediate segment in a consumer's layout (a `saves/`
directory, say) belongs to *its* root resolution and never appears here.

**Normalization maps; validation rejects.** `NormalizeSlotName` trims, collapses internal
whitespace runs to one space, drops control characters and the characters no path component may
carry (`/ \ : * ? " < > |`), and truncates to `MaxSlotNameLength` (32). It never fails and it never
folds case — folding would silently merge names differing only in case, and rename an existing
slot's directory the first time it was reopened. `IsValidSlotName` normalizes and then refuses the
empty result, the relative-path elements `.` and `..`, a trailing dot, and the Windows device names
(`CON`, `NUL`, `COM1`, … — matched case-insensitively and with any extension, and refused on every
platform so a slot list is not platform-dependent).

The two stay **separate functions** because folding them into one loses the `.`/`..` rejection, and
given `SlotDirectoryOf` that is a path-traversal hole rather than a cosmetic gap. For the same
reason **`SlotDirectoryOf` returns a `Result`, never a bare path**: the only path a rejected name
could resolve to is the root itself, which `OpenSlot` would then lock and sweep as though the whole
root were one slot. Since normalization has already dropped every separator, an accepted name is
always a single component directly under the root.

**`EnumerateSlots` reports filesystem facts and opens no store.** It skips non-directories, so a
root holding consumer files beside its slots (an account record, say) enumerates cleanly, and it
sorts on `SlotInfo::LastWriteWall` descending with the name breaking ties. That stamp is a
**directory mtime, advisory only** — not a semantic "when was this slot last saved", and the store
perturbs it, since `Store::Open` writes `slot.lock` and may sweep superseded files. A consumer
ordering a user-facing list on when the state was last written keys on its own persisted stamp;
sorting here is a sensible default for a bare directory scan, not a guarantee. Display metadata
beyond these facts is likewise the consumer's, layered through its own store family.

**`OpenSlot(root, name, createIfAbsent)`** resolves, optionally creates the directory, and calls
`Store::Open`. Every failure is a recoverable `Result` — an unusable name, an absent slot without
`createIfAbsent`, and whatever `Store::Open` reports passed through with its reason (lock
contention, an unreadable or unrecognized slot). `OpenSlot` is what makes the store's foreign-slot
refusal matter, since a consumer may hand it any root at all.

## Store patterns

`Veng/Persistence/StorePatterns.h` ships the two shapes a store consumer writes on day one, as
**constructors over the delivered surface** — they build ordinary `StoreFamily` values and call
ordinary `Store` methods, and a registrar whose logic genuinely diverges keeps hand-written hooks.
The reason they are public is not the line count they save: it is that their **edge semantics are
pinned and tested**, so consumers do not each invent their own answer to the same handful of
questions.

### The component-set family

`ComponentSetFamily<MarkerT, ComponentTs...>(id, fileStem, keyOf, types)` is "persist these
component types off every entity carrying this marker, keyed by an extractor". `Capture` walks the
marker set, derives each entity's key through `keyOf`, and writes every `ComponentTs` the entity
carries as a reflected blob; `RehydrateKeys` collects the same keys off a fresh scene and
`Rehydrate` applies a stored record's blobs onto the entity whose key matches. `Version` and an
optional `Migrate` stay the caller's to set before registering.

`keyOf` is a spelled-out `function`, not a deduced callable: `ComponentTs` is a pack the caller
always supplies explicitly, so a trailing deduced parameter would be a trap. `types` is captured by
reference and must outlive the store.

The pinned semantics — each one a place two hand-written registrars could reasonably differ:

- A `nullopt` key **skips** the entity; it is never written under a sentinel key.
- An entity from which **zero components were captured contributes no record**. The natural
  hand-written spelling walks a view of the marker *plus* the components, so a marker carried alone
  writes nothing; a helper emitting a record regardless would write an empty record per marked
  entity per capture and grow the family forever.
- **Rehydrate adds a stored component the claimant lacks**, rather than only updating what is
  already present — restoring onto a freshly built entity is the common case.
- A component type absent from a stored record leaves the live component untouched.
- A stored blob matching none of `ComponentTs` is **skipped and logged**, naming the type id, once
  per family. The latch is the family's own, so repeated rehydrates of the same drift cost one line
  rather than a flood — but never silence: an unrecognized blob is a diagnostic.
- **First claimant wins** where several entities resolve one key; the rest are left untouched. The
  claimant is resolved before anything is applied, since rehydrate adds components and a scene
  mutated mid-view would iterate a moving pool.
- Rehydrate is an **identity restore**: the elapsed wall seconds are forwarded to nothing. Catch-up
  math is a consumer's own `Rehydrate`, which stays fully supported.

Capture folds over the pack with a **comma fold, not a disjunction** — a short-circuit would drop a
component sitting behind an absent predecessor. Rehydrate's apply fold *is* a disjunction, because
exactly one type claims a blob.

### The singleton record

`SingletonFamily(id, fileStem)` builds a hook-less family holding one record at
**`SingletonRecordKey`** (the zero key) — the whole-slot settings shape, written directly rather
than captured off entities. `ReadSingleton<T>` returns `nullopt` when the record or `T`'s blob is
absent, or the blob failed to decode (logged).

`WriteSingleton<T>` is **read-modify-write at the blob level**: it reads the stored record back,
replaces or inserts `T`'s blob, and preserves every other blob in the record, so independent types
sharing the singleton never clobber each other. It is **not field-level** — writing a `T` replaces
the whole `T` — so a consumer holding several independently-updated fields inside one reflected type
still reads that type back and modifies it before calling. Stating it the other way round would
promise something the helper does not do.

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
