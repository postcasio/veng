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

## The local account store

`Veng/Persistence/LocalAccountStore.h` is the durable local identity of record: **one account per
root**, load-or-minted at `LocalAccountStore::Open(root)` and persisted before the call returns. It
sits above the slots — the account is one, the slots are many — and shares nothing with `Store` but
the lock mechanism.

**The engine interprets exactly one account field: the id.** `GetId()` is a `Net::AccountId`, the
value a consumer returns from `Net::GameNetInfo::Identity` (the store is the natural feeder; the
hook stays explicit and nothing auto-wires it). Everything else an application keeps on an account
— a display name, contact details, preferences — lives in the **profile**, a `Net::Blob` the store
persists beside the id and **never decodes**: the type tag and the bytes go in and come back out
verbatim. There is no engine-defined account field beyond the id and there is not going to be one.
The reflection record encoding is the recommended codec for the payload, its tolerant read being
what lets a consumer's profile fields accrete without a format break here. The store is **at-rest
only** — nothing in the engine moves a profile off disk.

Consumer id schemes ride two hooks on `LocalAccountInfo`: **`MintId`** (default
`Net::GenerateAccountId`) and **`ValidateId`** (default: any nonzero id). An application whose ids
carry an invariant of their own supplies both, so `AccountId` stays opaque in both directions.

### The id is irreplaceable, so surprise is never resolved by re-minting

Sessions key on the account id and a consumer's record families will too, so discarding it orphans
every account-keyed record on disk — and overwrites the evidence of why. "Re-mint on anything
unexpected" is therefore the wrong reflex, and the store splits it into four outcomes:

- **Absent record** → mint and write immediately, before the id is handed out.
- **Present and adoptable** → adopt. An ordinary open never changes the id.
- **Unparseable, or rejected by `ValidateId`** → rename the record to `<root>/account.corrupt`,
  preserving the bytes, *then* mint a replacement and write. The open succeeds and
  **`WasIdentityReset()` reports it** — a consumer tells the player its identity was reset rather
  than letting them discover it through orphaned records. If the bytes cannot be preserved, the
  open fails instead: a mint over unmovable evidence is worse than no store.
- **Present with a format version this build does not know** → **fail the open**, touching nothing.
  A newer record means the user has run a newer build, and silently replacing it turns a downgrade
  into permanent identity loss. This is the case a two-outcome rule folds into "unparseable".

`Open` returns a `Result` for the same reason a failed mint-write must be an error rather than a
degradation: a by-value return can only hand back a store reporting a valid id and
`IsEphemeral() == false` while nothing was persisted, so the id already given to `Identity` and
keyed on quietly becomes a different id next launch. That is the one invariant the class exists
for. **`Ephemeral()`** is the zero-config posture requested by name for the same reason — an empty
root path is not inferred as "in memory is fine", because a consumer whose root resolution failed
should get an error, not an identity that evaporates on exit. An ephemeral store reads, writes, and
locks nothing, and `SetProfile` on one succeeds having changed nothing.

### On disk

One small binary file, `<root>/account`: the eight-byte magic **`VNG.ACT1`**, a format version, the
id's `Lo`/`Hi`, then the profile (`TypeId`, byte count, bytes — a zero count is "never set"). It is
written through **`WriteFileAtomic`** (`Veng/Asset/AtomicFile.h`; `veng::assetpack` is linked
PUBLIC into `veng`) rather than a hand-rolled temp-and-rename, so a crash mid-write leaves the
previous record byte-identical and a stray temporary beside an absent record is inert. The profile
byte count is checked against the bytes remaining before it drives an allocation, as everywhere
else at this boundary.

`Open` holds an **exclusive advisory lock** on `<root>/account.lock` for the store's lifetime — the
same mechanism as the slot lock, and taken for the same reason: two processes of one application on
one machine is a shape consumers really run, two unlocked opens on an empty root both mint (and the
loser has already published its id), and two unlocked `SetProfile`s drop one.

### Rooting it beside the slots

The root is **consumer-supplied** — the store resolves nothing global, and any intermediate segment
in a consumer's layout belongs to that consumer's root resolution.
`Veng/Platform/UserPaths.h`'s per-user data directory is the natural provider, not a mandate.

Where a consumer roots the account file and its save slots together, the two must not collide: a
slot named `account` would resolve onto the record and the store would try to create a directory
over a regular file. The name **`account` is reserved** — slot-name validation rejects it, and slot
enumeration skips non-directories, which together also cover the store's other two files
(`account.lock` and `account.corrupt`, both extending the reserved name).
