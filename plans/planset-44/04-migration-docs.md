# Plan 04 — migrate JSON + fixtures + docs + C++ literals (the closer)

**Goal:** convert every id-bearing value in every checked-in JSON asset and test fixture to the
hex-string form, reformat the minted C++ id literals to the zero-padded spelling, and update
the prose. This is the atomic other half of the hard cut — it lands with Plans 01–03 so the
tree is never in a state where a reader rejects its own assets. Depends on Plans 00–03.

## 1. The JSON migration — driven by the reliable tool

A one-shot migration script — **committed at `scripts/migrate_ids.py`** (not thrown away after
the run), so a future out-of-repo pack can be converted with the same tool — converts the
id-bearing keys. **Reliability rule, per the user constraint:** the script parses each id with
Python's arbitrary-precision `int(...)` and re-emits `"0x%016X"` — **never** `float`, and never
a regex that rewrites digit runs in place (which cannot know a value's width or type). It walks
the JSON with `json.load`/`json.dump` so structure and key order are preserved by an
`object_pairs_hook`, and it edits **only** the id keys named below, leaving every other integer
untouched.

**The convert / keep-numeric decision is scoped per file schema, never a single flat key list.**
The same key name means different things in different files: `parent` is an `AssetId` in
`.vmatinst.json` but an **entity index** in `.prefab.json`; `id` is a minted `AssetId` in a pack
manifest but an entity-local index in a `.prefab.json`. The script keys its rules on
`(file glob, key)` pairs — e.g. `(*.vmatinst.json, parent)` converts, `(*.prefab.json, parent)`
stays numeric — so a name collision can never convert the wrong file's key.

**Keys that convert (value is a minted id):**
- pack manifest entry `id`; `startupLevel`; `defaultInstance`.
- shader `vertex_layout`; material `shaders.vertex`/`fragment`, texture field `id`; material
  `parent` and texture-override ids (bare and `{ "id": … }`).
- mesh `materials` **values** (not keys); mesh `skeleton`.
- level `world`; level `systems[]` entries (SystemId).
- inputmap action `id`; binding `action` (ActionId).
- reflected component `AssetHandle` fields inside `.prefab.json` / `.level.json` — arbitrary
  (game-defined) field names, so no `(glob, key)` rule reaches them. These are **hand-migrated**
  (see below); there are only six such files in the tree, and no reliable static heuristic exists
  (a "large integer under a component object" would misfire on a legitimate large non-id `u64`
  gameplay field, so it is **not** used).
- node-graph asset properties inside `.graph.json` — the same game-defined-key shape. No
  checked-in `.frag.graph.json` currently carries an asset property, so there is nothing to
  convert today; the script leaves `.graph.json` untouched and the post-sweep guard confirms it
  stays clean. (Stated explicitly so a future graph asset property is not silently missed.)

**Keys that must NOT convert (kept-numeric allowlist, encoded in the script):**
- prefab entity `id` (`EntityIdKey`), component-array `index`, `parent` entity index.
- inputmap `control`; pack `version`; texture/environment `max_size`; animation `clip` /
  `trimStart` / `trimEnd`; material field scalar `value`; mesh `materials` map **keys**.

**The prefab/level files are hand-migrated, not machine-swept.** A re-serialize round-trip
cannot run in this planset's sequence: the writer must first *read* the old numeric form, but
after Plan 01 the reader accepts **only** hex — a chicken-and-egg — and no `vengc` re-serialize
subcommand exists (the subcommands are `cook` / `cook-project` / `generate-id` /
`generate-type-id` / `verify`; building a headless prefab re-serializer is its own non-trivial
task, out of scope here). The corpus is **six files** with a handful of `AssetHandle` fields
each — hand-edit them and eyeball the diff:
`examples/hello-triangle/assets/prefabs/scene.prefab.json`, `.../player.prefab.json`,
`examples/hello-triangle/assets/levels/sample.level.json`,
`examples/template/assets/prefabs/scene.prefab.json`,
`examples/template/assets/levels/scene.level.json`, and
`tests/cooker/fixtures/prefabs/scene.prefab.json`. Manifests, `project.veng`, and the flat
per-asset sources (shader/material/mesh/inputmap/level-wrapper) have fixed key names and take the
direct script.

**Files in scope:**
- `engine/assets/core/core.vengpack.json` and every core per-asset source under
  `engine/assets/core/`.
- `editor/assets/**` — the editor icon pack
  (`editor/assets/icons/editor_icons.vengpack.json`, whose `id`s are themselves `> 2^53`) is a
  real id-bearing manifest cooked at build time via `veng_add_asset_pack`. Without migrating it,
  the **default in-tree editor build breaks after Plan 02** flips the manifest read — and the
  three-root guard below would not even catch it. Easy to miss because it lives outside
  `engine/assets`/`examples`/`tests`.
- `examples/hello-triangle/` and `examples/template/` — every `*.vengpack.json`, `project.veng`,
  and per-asset source. Both examples co-migrate here (working norms).
- `tests/**` — every fixture pack and per-asset source under `tests/cooker/fixtures/`,
  `tests/editor/`, `tests/shaders/`, and any golden-adjacent JSON. A fixture that tests the
  *malformed-id* case updates its expected error to the new "expected a hex-string id" message.

**Post-sweep guard** (must all come back empty):
```
rg '"(id|startupLevel|defaultInstance|vertex_layout|world|parent|action|skeleton)":\s*[0-9]' \
   --glob '*.json' --glob '*.veng' engine/assets editor/assets examples tests
rg '"(vertex|fragment)":\s*[0-9]' --glob '*.vmat.json' --glob '*.json' engine editor examples tests
```
Eyeball the known **kept-numeric** false positives, which live in `.prefab.json` /
`.inputmap.json` and stay numeric by design: prefab entity `id`, prefab `parent` **and** `index`
(entity indices — note `parent` appears in the guard regex but is an id only in `.vmatinst.json`),
inputmap `control`, and pack `version`. Better than eyeballing: have `scripts/migrate_ids.py` emit
a machine-readable list of every `(file, line, key)` it intentionally left numeric, and diff its
line count against the expected count, so a genuine miss can't hide among expected positives.

## 2. The C++ literal reformat

Reformat **minted** id literals to `0x` + 16 uppercase digits (`0x0D49F2A1C03B5E76ULL`). These
are the core-pack layout ids and any other real minted `AssetId`/`TypeId`/`SystemId`/`ActionId`
`0x…ULL` constants in engine/example source. **Leave the small test sentinels** (`AssetId{7777}`,
`AssetId{0x3E9}`, `AssetId{4242}`, `AssetId{9001}` in `tests/cooker/`) as authored — they are not
minted ids and padding them to 16 digits is noise. A minted literal is identifiable by being a
full-width random value (already >8 hex digits); when in doubt, leave it.

## 3. The prose

- **Root [CLAUDE.md](../../CLAUDE.md)** — the "Hardcoded `AssetId` literals … uppercase
  hexadecimal with a `0x` prefix" paragraph gains "**zero-padded to 16 digits**", and the
  "JSON asset packs keep decimal ids" clause flips to "JSON stores ids as the same
  zero-padded hex **string**". Update the `generate-id` description (prints both hex forms, no
  decimal).
- **[assetpack/CLAUDE.md](../../assetpack/CLAUDE.md)** — the `AssetId` bullet: opaque `u64`,
  addressed in JSON as a canonical hex string via `Veng/Asset/HexId.h`.
- **[cooker/CLAUDE.md](../../cooker/CLAUDE.md)** and the
  [Cooker.h](../../cooker/include/Veng/Cook/Cooker.h) header comment (`"startupLevel" is a
  decimal AssetId`) → hex string.
- **[engine/CLAUDE.md](../../engine/CLAUDE.md)** — the reflection/`TypeId` prose that says
  "hex in C++ / decimal in JSON" for the parallel id spaces → "hex in both".
- **`mcp`/editor docs** — the MCP asset-id argument/response is now the hex string (Plan 03).
- **`docs/guides/`** — `wiring-a-level.md`, `writing-gameplay-systems.md`, and any
  asset-authoring example that shows a numeric id → hex string; `consuming-veng.md` if it
  shows a pack manifest.
- **The `id_comments` memory** note — refresh so it reflects the zero-padded-16 convention if
  it references the id spelling.

## Verification (the full band, run only after this plan lands)

- Clean `build-debug`, `-Werror`; full `ctest` green — the `cooker`, `unit`, `editor`, and
  `death` suites re-cook and round-trip every migrated fixture.
- **Semantic (not just pixel) equivalence.** The golden image (below) only exercises the
  hello-triangle render path; most migrated fixtures — `tests/cooker/fixtures/*`, `tests/editor/*`,
  inputmaps, unrendered levels, the editor icon pack — never reach it, and a fixture wrongly
  converted (or wrongly left numeric) that still *parses* would not move a pixel. Confirm each
  such fixture is actually loaded by the suite that owns it (so a silent mis-migration fails a
  test, not just the golden), and for the packs that cook, spot-check a before/after byte-diff of
  the cooked blob is empty apart from the id-encoding bytes.
- `smoke_golden` and `hello_triangle_launcher_smoke` — the migrated core/example packs cook and
  render **identically**; this change moves no rendered pixel, so the golden must **not** change.
  A re-cook of the core pack is involved: watch the ccache `#embed` staleness trap — delete the
  core-pack embed `.o` (or `CCACHE_DISABLE=1`) after the format migration or the runtime mounts
  a stale pack and fails with "asset … not found".
- The `validation` gate (`ctest -L validation`) — green, since the render path is unchanged.
- `sdk_conformance_install` / `sdk_conformance_buildtree` — the template's out-of-tree cook
  consumes the migrated `project.veng` + pack.
- The two post-sweep `rg` guards come back empty.

## Out of scope

- Nothing — this is the closer. Any remaining numeric id is a bug this plan must catch.
