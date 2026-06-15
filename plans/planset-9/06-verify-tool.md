# Plan 06 — `vengc verify`

> **Stream C (archive content hashes), plan 2 of 2.** Depends on plan 05; cooker-side
> only, no shared files with streams A or B.

**Goal:** add a `vengc verify <archive>` subcommand that re-hashes a cooked archive and
reports any mismatch — the on-demand integrity check the loader deliberately does not do.
Cooker-side only; the engine never links it.

## Why this is its own plan

Plan 05 produces the hashes; this plan is the consumer that proves they are correct and
gives the format its payoff (integrity verification). Separating it keeps the format/write
change (05) focused and lets `verify` be tested as a tool — including against a
deliberately corrupted archive — without touching the producer.

## The subcommand — `vengc verify <archive.vengpack>`

Beside the existing `cook` / `generate-id` subcommands (same CLI dispatch):

1. `ArchiveReader::Open(path)` — a `VersionMismatch` (e.g. a stale v1 pack) or unreadable
   file is reported and exits nonzero before any hashing.
2. For each TOC entry, fetch its blob bytes (`Find(id)`), recompute **xxh3-128** over them,
   and compare to the entry's stored `Hash`.
3. Recompute the **archive digest** by hashing `ArchiveReader::TocBytes()` (the raw
   serialized TOC byte region) with the vendored xxh3-128 and compare to the header's
   `ArchiveDigest()`. Hashing the exact on-disk bytes the cooker hashed makes the
   comparison valid with no re-sorting or re-serialization — and catches TOC-level
   tampering (reordered entries, edited `id`/`type`/`offset`/`size`, or a per-blob hash
   field altered to mask a swapped blob) that re-hashing the blobs alone cannot.
4. Print a per-asset report — `OK` / `MISMATCH` with the asset id and type — and a summary.
   **Exit nonzero if any blob or the digest mismatches**, zero if all clean. (A zero stored
   hash — an archive written by a non-cooker tool — counts as a mismatch and is named.)

The tool reuses the cooker's vendored xxHash; it links `libveng_cook` / `assetformat`,
never `libveng`. The verify logic lives as a **function in `libveng_cook`** (e.g.
`VerifyArchive(path) -> result/exit code`); the `vengc verify` CLI is a thin wrapper over
it. This lets the tests below call it **in-process** — the same way the existing cooker
round-trip cases drive `libveng_cook` directly, with no `vengc` subprocess to spawn.

## Tests — the cooker cases in `veng_unit`

Driver-free, alongside the existing cooker round-trip cases (compiled into `veng_unit`,
run under `ctest -L unit` — there is no separate `cooker` label):

- **Round-trip:** cook a small pack (plan 05 path) → `verify` exits 0 and reports every
  asset `OK`.
- **Blob corruption:** flip a byte in a cooked blob's region of the archive bytes →
  `verify` exits nonzero and names exactly the affected asset; an intact pack beside it
  still passes.
- **Digest-only:** tamper the header's stored `ArchiveDigest` bytes directly, leaving every
  blob and the TOC intact → all per-blob hashes report `OK` but the recomputed TOC digest
  disagrees with the stored one, so the digest reports `MISMATCH` and the tool exits
  nonzero. This isolates the digest comparison: a blob-byte flip is caught by the per-blob
  pass (the TOC is unchanged, so the digest still matches), so corrupting the stored digest
  is the clean way to drive the digest path alone.
- **TOC tamper:** edit a TOC field in the archive bytes — e.g. flip a byte in an entry's
  stored per-blob `Hash` or its `offset` — without touching the header digest → the
  recomputed TOC digest reports `MISMATCH` and the tool exits nonzero, proving the digest
  guards the table of contents (the integrity the per-blob pass does not provide).
- **Stale version:** a v1-era fixture (or a hand-built wrong-version header) → `verify`
  reports `VersionMismatch` and exits nonzero without hashing.

## Acceptance

- `vengc verify <archive>` re-hashes and reports per asset, with the right exit code (0
  clean, nonzero on any mismatch / unreadable / version drift).
- The new cases (round-trip + blob corruption + digest-only + TOC tamper + version drift)
  run under `ctest -L unit`; `ctest` green.
- The engine builds and links unchanged — `verify` is cooker-only, never reaching
  `libveng`.
