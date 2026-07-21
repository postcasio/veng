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

## The session-store binding

## The local account store
