# planset-22 — recoverable reflection deserialization

**Phase goal:** make the reflection serializer's **read** side **recoverable** instead of
fatal. `ReadFields` today aborts via `VE_ASSERT` the moment it meets a truncated record
([Serialize.cpp](../../engine/src/Reflection/Serialize.cpp)); the runtime prefab loader feeds
it cooked-blob bytes and the editor's cook-on-demand path feeds it mid-edit bytes, so a single
malformed record takes the whole process down. Turn the **byte-level truncation guards** into a
`VoidResult` so a corrupt record is a value, not an abort — letting the prefab loader surface it
as the structured `AssetError::Corrupt` it already raises for its own range checks, and letting
the unit band assert on malformed input **in-process** (which the separate-process death harness
cannot do for graceful-recovery behavior).

The change is deliberately **narrow**, drawing the one line veng's error policy already implies:
**bad bytes are recoverable, a broken schema is not.** The `cursor + n <= in.size()` truncation
checks become recoverable errors; the `registry.Info(field.Type)` schema/registration lookups
stay fatal asserts (an unregistered type is API misuse, not bad data — the
[CLAUDE.md](../../CLAUDE.md) "API misuse → `VE_ASSERT`, recoverable → `Result<T>`" split). The
**write** side is untouched: `WriteFields` serializes a live, registered object and structurally
cannot meet bad bytes.

## Scope decisions

1. **`ReadFields` returns `VoidResult`; `WriteFields` stays void.** The read side parses
   external bytes that may be truncated — the textbook recoverable case, the same category as
   "loading a shader file that may not exist." The write side appends a registered in-memory
   value's bytes to a vector; its only failure mode is an unregistered nested type, which is the
   schema-assert below, not a byte error. So only the reader gains a `Result`.

2. **Truncation is recoverable; a broken schema stays fatal.** Every `cursor + n <= in.size()`
   guard (truncated u32 / leaf / string / asset id / field name / field value) becomes
   `return std::unexpected(...)` carrying a located message. The `registry.Info(field.Type)`
   lookups — reached only when a descriptor names a type the registry does not hold — stay
   `VE_ASSERT`: an unregistered component/leaf type is registration misuse the host must fix, not
   data the runtime should tolerate. The line is **bad bytes vs. broken schema**, and it keeps
   the change scoped to the truncation arithmetic.

3. **The loader propagates it as `AssetError::Corrupt`.** The prefab loader already returns
   `std::unexpected(Corrupt(id, "..."))` for its own structural range checks
   ([PrefabLoader.cpp:177](../../engine/src/Asset/Loaders/PrefabLoader.cpp),
   [:187](../../engine/src/Asset/Loaders/PrefabLoader.cpp)); a `ReadFields` failure joins them as
   one more `Corrupt`, surfacing through `LoadSync`'s existing `AssetResult` channel. A corrupt
   cooked prefab becomes a recoverable load failure, not an abort — no new error channel, and the
   "the runtime trusts its hashed packs" stance is unchanged for a *valid* pack (trust is about
   not re-hashing, not about treating corruption as unrecoverable).

4. **The load-time parse validates; the spawn-time parse asserts.** `ReadFields` runs twice on
   a record: once at **load** (collecting embedded handle deps,
   [PrefabLoader.cpp:206](../../engine/src/Asset/Loaders/PrefabLoader.cpp)) and once at **spawn**
   ([Prefab.cpp:139](../../engine/src/Asset/Prefab.cpp)). The load-time call is the
   untrusted-first-parse and returns the `AssetResult`; by spawn time the record has already
   parsed clean at load, so the spawn-side call `.value()`s it (an error there would be an engine
   bug, not bad data). So `Prefab::SpawnInto` keeps its `vector<Entity>` return — no signature
   churn rides this change.

5. **No on-disk format change, no `WriteFields`/round-trip change.** The byte encoding is
   identical; a pack cooked before this planset reads identically after it. The only behavioral
   difference is that a record that previously **aborted** now **returns an error** — every valid
   input is byte-for-byte unchanged, so the smoke golden never moves.

6. **The three serializer death tests are deleted, not converted.** `VoidResult` is not
   `[[nodiscard]]`, so the change does **not** force the happy-path read callers (13 in
   `tests/unit/reflection.cpp`, 6 in `tests/cooker/prefab_cook.cpp`, the two engine sites) to
   compile-change. The one forced test move is the death band:
   `serialize_truncated_header/value/string`
   ([death_main.cpp:226–279](../../tests/death/death_main.cpp),
   [CMakeLists.txt:358–367](../../CMakeLists.txt)) assert that malformed bytes **abort** — the
   exact contract reversed here — so they are removed (functions, dispatch arms, `add_test`
   blocks, the now-false comment) and their coverage migrates to the in-process `.error()`
   assertions in `reflection.cpp`. All three are truncation cases, so none survive as a converted
   assert. This — plus the loader propagation (Plan 02) — is the entire forced blast radius of the
   signature change.

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 01 | [ReadFields returns a Result](01-serializer-result.md) | `ReadFields` → `VoidResult`; the internal `ReadFieldsInner`/`ReadValue`/`ReadU32` truncation guards return `std::unexpected` instead of `VE_ASSERT`; the `registry.Info` schema lookups stay fatal; `WriteFields` untouched. In-process unit tests over malformed/truncated bytes (assert on `.error()`) plus the drift-skip recovery cases. | done |
| 02 | [Propagate to AssetError::Corrupt](02-loader-propagation.md) | The prefab loader's load-time `ReadFields` ([PrefabLoader.cpp:206](../../engine/src/Asset/Loaders/PrefabLoader.cpp)) propagates a read failure as `Corrupt(id, ...)`; the spawn-time call ([Prefab.cpp:139](../../engine/src/Asset/Prefab.cpp)) `.value()`s the already-validated record. A `gpu`-band test feeds a truncated cooked prefab blob and asserts `LoadSync` returns `AssetError::Corrupt`, not an abort. | done |

_(Companion items are open — this planset can grow a row or two before it lands.)_

## Dependency analysis

```
01 (ReadFields -> VoidResult) ──► 02 (loader propagates Corrupt)
```

Serial. Plan 01 is the pure, device-free core — the signature change and its in-process malformed-input
tests land their value with no GPU. Plan 02 threads the new `Result` through the one consumer that
parses untrusted-first bytes and pins the end-to-end behavior with a `gpu`-band corrupt-blob test.

## Process & conventions

Same cadence as every planset: implement → migrate every `ReadFields` caller in the same pass
(the cooker uses only `WriteFields`, so only the two engine read sites move) → verify (clean
build, `ctest` green across the unit/death/cooker bands and the `gpu` band where a device is
present, the smoke PPM correct size + exit 0, `smoke_golden` **byte-identical**) → update this
table → one commit per plan (`Plan NN: <summary>`, `Co-Authored-By` trailer).

Common to both plans:

- **`smoke_golden` stays byte-identical.** No encoding changes; every valid input reads the same
  bytes. A moved golden is a bug.
- **The new behavior is tested in-process, replacing the death cases.** The point of the
  `Result` is that malformed input no longer aborts — so the malformed-input cases move to the
  **unit** band (`reflection.cpp`) and the **gpu** band (the corrupt-prefab `LoadSync`), asserting
  on the returned error, and the three `serialize_truncated_*` **death** cases are deleted (their
  abort contract no longer holds). The other death tests for genuine misuse (a stale `Entity`, an
  unregistered type at spawn) are unaffected.
- **Run the validation gate under `VE_DEBUG`** — no render path changes, so the gate is a
  no-regression check, not a new surface.
- **Contract comments are present-tense facts** — the CLAUDE.md comment policy applies; no "used
  to abort" / "before this was fatal" narrative. State the current rule: truncation is a
  recoverable error, a broken schema is fatal.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.

## On completion

The reflection serializer's read side is **recoverable**: `ReadFields` returns a `VoidResult`,
truncated bytes produce a located error rather than an abort, and a broken schema stays a fatal
assert. The prefab loader surfaces a corrupt cooked blob as `AssetError::Corrupt` through the
existing `AssetResult` channel, the editor's cook-on-demand path no longer risks a crash on a
mid-edit record, and the malformed-input and drift-recovery paths are pinned by in-process tests
the abort previously made untestable. The on-disk format and the smoke golden are unchanged.
