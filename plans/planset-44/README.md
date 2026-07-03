# planset-44 — portable asset ids: 64-bit ids as zero-padded hex strings

**Phase goal:** no 64-bit identifier is ever stored in JSON as a bare number again. Every
minted id — `AssetId`, `SystemId`, `ActionId` — serializes to and from JSON as a
**zero-padded hex string** (`"0x0D49F2A1C03B5E76"`), and the hardcoded C++ literal
convention is tightened to the identical zero-padded spelling, so the JSON and the C++
forms are byte-for-byte the same string modulo the quotes and the `ULL` suffix.

## Why now

- **It is a correctness bug, latent today.** A minted id is a random full-range `u64`; the
  values already in the tree run well past `2^53` (`15320822976973617713` in
  [core.vengpack.json](../../engine/assets/core/core.vengpack.json)). Stored as a **bare JSON
  number** — which is what every id-bearing key does today — any consumer that parses JSON
  numbers as IEEE-754 doubles (jq, most editors' JSON tooling, any JS/Python-float pipeline,
  and `nlohmann` itself the moment a value ever routes through `double`) silently truncates
  it. We dodge it only because `nlohmann` happens to park unsigned integers in a `u64` slot;
  the on-disk representation is not portable, and the format is meant to be
  (`consuming-veng` ships packs as data). Hex strings round-trip through *any* JSON tool
  losslessly, because a string is a string.
- **The bug already bit once, in-tree.** `mcp/src/ReflectToJson.cpp` ships a whole
  `ConvertAssetHandles` layer that re-encodes every `AssetId` to a **decimal string** on the
  wire, with a doc comment naming this exact `2^53` loss — the MCP surface hit the truncation
  and worked around it locally. This planset makes the walker emit the portable form natively,
  so that workaround is **deleted** (Plan 01): the change pays for itself in ~100 fewer lines
  of tree-walking re-encoder, not just future-proofing.
- **One representation, everywhere.** The codebase already writes C++ id literals in
  uppercase hex (`AssetId{0x4DD9F2A1C03B5E76ULL}`); JSON was the lone decimal holdout
  (`vengc generate-id` prints both forms precisely because "JSON has no hex literal"). Making
  JSON hex — and zero-padding both sides to 16 digits — collapses the two spellings into one:
  a minted id reads identically in a `.vengpack.json`, a `.prefab.json`, and a C++ source
  file, and `generate-id`'s output pastes into either verbatim.
- **The id surface is fully mapped and mechanical.** Every read is
  `is_number_unsigned()` + `get<u64>()`; every write assigns a `.Value`. The change is a
  find-and-convert against a closed, enumerated set of sites, funnelled through **one shared
  codec**, with a hard rule for the integers that are *not* ids.

## The one hard rule: not every JSON integer is an id

Only the **minted 64-bit identity types** convert — `AssetId`, `SystemId`, `ActionId`. These
JSON integers are **not** minted ids and stay numeric; a migration that touches them is a
bug:

- **prefab entity `id`** (`EntityIdKey`) — a small entity-*local* index, not an `AssetId`
  ([PrefabImporter.cpp:199](../../cooker/src/Importers/PrefabImporter.cpp),
  [PrefabSerialize.cpp:220](../../editor/src/PrefabSerialize.cpp)).
- **prefab component-array indices / `parent`** — entity-local indices
  ([PrefabImporter.cpp:104](../../cooker/src/Importers/PrefabImporter.cpp)).
- **inputmap `control`** — a raw device scancode/keycode
  ([InputMapImporter.cpp:169](../../cooker/src/Importers/InputMapImporter.cpp)).
- material-field std140 offsets, `max_size`, `clip`/`trimStart`/`trimEnd`, mesh material-slot
  **map keys** (decimal-string keys, unrelated), pack `version` — all untouched.

`TypeId` is **not** in scope and has no bug: it never reaches JSON as a number. Variant tags
and component types serialize by **name** (`{ "type": "Veng::vec3" }`), a string that
round-trips perfectly; keeping the readable name as the type key is a deliberate authorability
choice, and type-rename resilience — if ever wanted — is a separate alias-table pass, not
opaque ids in hand-authored files.

## The unifying design

### One codec in `assetpack`, built on the reliable tool

`assetpack` is the one library every consumer already links — the cooker importers (which do
**not** all link `veng`), the engine walker, the editor, and `veng::graph` — and it already
owns `AssetId`. The codec lives there:

```cpp
// assetpack/include/Veng/Asset/HexId.h  — JSON-free, no fmt in the header
namespace Veng
{
    /// @brief Formats a 64-bit id as the canonical "0x" + 16-uppercase-hex-digit string.
    string FormatHexId(u64 value);

    /// @brief Parses a hex id string (optional 0x/0X prefix, case-insensitive) to a u64.
    ///
    /// Returns nullopt on empty input, overflow past 64 bits, or any non-hex or trailing
    /// character — the reader turns nullopt into a located error.
    optional<u64> ParseHexId(string_view text);

    /// @brief Typed AssetId wrappers over the u64 codec.
    string FormatAssetId(AssetId id);
    optional<AssetId> ParseAssetId(string_view text);
}
```

The **reliable tool** is deliberate and enforced:

- **Format** uses `fmt::format("0x{:016X}", value)` (fmt is already a PRIVATE dep of
  `assetpack`, used in its `.cpp`s; the header stays fmt-free). `{:016X}` gives the exact
  zero-padded 16-digit uppercase spelling with no manual padding.
- **Parse** uses `std::from_chars(first, last, value, 16)` after skipping an optional `0x`/`0X`.
  `from_chars` is the only correct choice under `-fno-exceptions`: locale-independent,
  exception-free, full 64-bit range, and it reports both an `errc` **and** a `ptr`, so a
  trailing-garbage string (`"0x12cat"`) is rejected rather than silently partial-accepted.
- **Banned** in this work: `std::stoull` (throws), `std::strtoull` (locale/errno, silent
  partial-accept — the one existing offender at
  [PrefabImporter.cpp:261](../../cooker/src/Importers/PrefabImporter.cpp), which is an
  entity-index parse and stays numeric anyway), any stream, and — the whole point — any path
  that routes a 64-bit id through a JSON `double`. The migration script (Plan 04) obeys the
  same rule: Python arbitrary-precision `int` only, never `float`, never a digit-rewriting
  regex.

### The canonical format, C++ and JSON alike

`0x` + exactly **16 uppercase hex digits**. JSON: `"0x0D49F2A1C03B5E76"`. C++:
`0x0D49F2A1C03B5E76ULL`. Zero-padding matters because ~1/16 of minted ids have a leading zero
nibble, so without it the two spellings drift. The zero-pad rule applies to **minted** ids;
the deliberately-tiny **test sentinels** (`AssetId{7777}`, `AssetId{0x3E9}`) are not minted
ids and are left as authored.

### Hard cut, no numeric fallback

Readers accept the hex-string form **only**; a leftover bare number is a loud, located error
naming the field. Every id-bearing JSON asset and fixture in the tree migrates in the same
planset. Rationale: it matches the house posture (stale/foreign forms fail loudly); at `0.1.0`
(pre-tag, no known external consumer) there is no external corpus to be lenient toward — a fact
of the project's current maturity, not a structural guarantee, so the migration script (Plan 04)
is **committed at `scripts/migrate_ids.py`** and can convert any future out-of-repo pack; and a
tolerant reader would keep the precision bug alive on the number branch forever. The cut lands
atomically — the reader plans (01–03) and the asset-migration plan (04) ship together, and the
two examples co-migrate per the working norms.

**Landing discipline.** The readers (01–03) and their on-disk assets (04) must move together, and
several assets are read by more than one plan (the core manifest by both the cooker in Plan 02 and
the editor's `AssetSourceIndex` in Plan 03), so a per-plan asset co-migration cannot keep every
intermediate commit green. The planset therefore lands on a **feature branch** and merges to `main`
only once Plan 04 is green: individual per-plan commits may be red *on the branch*, but `main` is
never left with a red `ctest` or a poisoned `git bisect`. There is **no CI gate** in this repo (the
pre-commit hook runs only format/tidy), so this discipline is manual — do not push a red state to
`main`.

## Plans

**Dependency order:** 00 first; 01 and 02 depend only on 00 and are independent of each other
(may run in parallel); 03 depends on 00 and is sequenced after both 01 and 02 (it shares their
walker and cooker conventions); 04 lands last, depending on 00–03.

| # | Plan | Summary | Status |
|---|------|---------|--------|
| 00 | The hex-id codec + zero-padded convention | `assetpack/Veng/Asset/HexId.h` (`FormatHexId`/`ParseHexId` on `fmt`+`from_chars`, typed `AssetId`/`ActionId` wrappers), unit-tested (round-trip, zero-pad, overflow/trailing-junk rejection, case tolerance, `0x0` → invalid, the `0xFFFFFFFFFFFFFFFF` boundary). `vengc generate-id` prints `0x{:016X}ULL` (C++) and `"0x{:016X}"` (JSON). No consumer converts yet. | done |
| 01 | The two reflective walkers (+ MCP) | `JsonSerialize.cpp`'s `ReadAssetHandle`/`WriteAssetHandle` and `NodeGraphSerialize.cpp`'s `AssetHandle` case move to `ParseAssetId`/`FormatAssetId` (string, not number); id 0 now serializes as `null` on the write side too. Covers every prefab/level component `AssetHandle` field and every node-graph asset property at once. Also **deletes `mcp/src/ReflectToJson.cpp`'s `ConvertAssetHandles` decimal-string layer** (walker-adjacent; it breaks the moment the walker flips) and moves MCP's wire form to the hex string. Depends on 00. | done |
| 02 | Cooker importers + manifest | Every id read/write in `cooker/src/`: manifest `id` (**both** the `ParseAssetPack` read *and* the separate `CookEntry` per-asset read), `startupLevel`, `defaultInstance`, shader `vertex_layout`, material `shaders`/texture-field `id`, mesh `materials` values + `skeleton`, level `world` + `systems[]`, inputmap action `id`/binding `action`, material-instance `parent` + texture overrides; the cooker's own JSON writes (`MaterialCompile`, synthesized default instance). `ActionId` and `SystemId` convert alongside `AssetId`. Depends on 00. | done |
| 03 | Editor writers + readers | The authoring side that produces the new on-disk form: `ProjectSettingsPanel`, `MaterialEditorPanel`, `MaterialInstanceEditorPanel`, `InputMappingEditorPanel`, `LevelEditorPanel` (`systems[]` write), plus **every** matching read — `EditorHost`, `AssetSourceIndex`, `MaterialEditorPanel` (its `defaultInstance` + `shaders` reads, easy to miss since it's also a write site), `LevelEditorPanel` (`systems[]` read), `MaterialInstanceEditorPanel`, `InputMappingEditorPanel`, `EditorMcp` (rewrite its file-local `ParseAssetId` body + all four id-output sites). `PrefabSerialize`'s entity id stays numeric. Depends on 00; sequenced after 01/02. | proposed |
| 04 | Migrate JSON + fixtures + docs + C++ literals | The reliable-script sweep (committed `scripts/migrate_ids.py`, per-file-schema key rules) over every `*.vengpack.json`, `project.veng`, and per-asset source under `engine/assets`, **`editor/assets`** (the icon pack), both examples, and all `tests/**` fixtures; the six `.prefab.json`/`.level.json` files are **hand-migrated** (game-defined field names + the post-Plan-01 reader defeat an automated round-trip); reformat minted C++ id literals to zero-padded 16 digits; prose pass (root/assetpack/cooker/engine `CLAUDE.md` convention, `docs/guides/`, cooker header comments, the `id_comments` memory). The closer. Depends on 00–03. | proposed |

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = implemented, migrated, verified, committed.

## Design decisions

- **Codec home is `assetpack`, not `engine`.** It is the lowest shared library across *all*
  consumers — several cooker importers link `assetpack` without linking `veng`, so a codec in
  `engine` would be unreachable from them. `assetpack` already owns `AssetId`, stays
  Vulkan/JSON-free (the header exposes only `string`↔`u64`, no `nlohmann`), and uses fmt
  privately already.
- **`from_chars`/`to_chars`, never `stoull`/`strtoull`/streams.** The user constraint —
  numeric conversion through a reliable tool — is the design's spine: exception-free (mandatory
  under `-fno-exceptions`), locale-independent, full-range, and trailing-garbage-rejecting.
- **Zero-pad both C++ and JSON to 16 digits.** The whole payoff is one spelling; drift between
  a 15-digit C++ literal and a 16-digit JSON string would undercut it. Scoped to minted ids;
  test sentinels stay small.
- **Hard cut.** No dual number/string tolerance to carry forever; the number branch is exactly
  what the precision bug lives on.
- **`TypeId` stays by name in JSON.** No precision bug (it is never a JSON number), and a
  readable type key beats an opaque id in hand-authored source. Type-rename resilience already
  holds at the cooked/runtime layer (binary `TypeId`); source-layer rename resilience, if ever
  wanted, is a separate alias-table pass.

## What remains future

- **A rename/alias table for type names in source JSON** — the mechanism to make a C++ type
  rename non-breaking for hand-authored prefabs/levels without giving up the readable name key.
  Deliberately out of this planset.
- **A generated JSON-Schema / editor-completion surface** that could validate the hex-id
  string shape — a natural later consumer, not built here.
