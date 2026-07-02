# Plan 04 — docs, conformance check, roadmap pass

**Goal:** the closer. Documentation catches up with the new posture (one JSON walker,
enums by name, nlohmann PUBLIC, the respelled string forms), the dual-example conformance
band runs end to end, and the planset closes out. Depends on Plans 00–03.

## What lands

### 1. `CLAUDE.md` updates

- **Root `CLAUDE.md`:** the dependency paragraph — nlohmann/json moves from
  "cooker-only / PRIVATE everywhere" to a PUBLIC dep of `libveng` (joining glm/fmt/ImGui in
  the `include_hygiene` link set); the "Build configurations" section's *"the JSON lives
  entirely in the consumers … so `libveng` gains no JSON dependency"* claim is rewritten to
  the new posture (the engine owns the JSON⇄reflection walker; cooked blobs stay binary and
  the runtime load path still parses no JSON).
- **`engine/CLAUDE.md`:** the reflection section documents `JsonSerialize.h` beside the
  binary serializer (the hook seam, the dotted-path errors) and the enum-by-name convention
  (`EnumName.h`'s runtime-typed functions, exact spellings, hard cut).
- **`cooker/CLAUDE.md`:** PrefabImporter/LevelImporter as walker call sites (validation and
  entity-remap as hooks); the respelled manifest `"type"`, `"domain"`, and `"compression"`
  vocabularies wherever they're quoted.
- **`editor/CLAUDE.md`:** `PrefabSerialize` as the walker's write inverse.
- **`mcp/CLAUDE.md`:** the enum output shape change (bare enumerator name, no
  `{ value, name }` object) and the strings-only mutate contract.
- **`assetpack/CLAUDE.md`:** the manifest `"type"` spellings, if quoted.

### 2. `docs/guides/` pass

- `consuming-veng.md`: nlohmann/json in the dependency list (FetchContent-pinned, exported
  with the SDK, `find_dependency` in `veng-config`).
- Every guide or example snippet quoting asset JSON updates to the new forms: enum names in
  prefab/level snippets, `"Surface"`/`"PostProcess"`, the manifest type names, the codec
  escape hatch. Sweep the `docs/` tree with the Plan 03 spelling grep rather than trusting
  memory.

### 3. The verification band

- Clean `build-debug` configure + build; full `ctest --test-dir build-debug
  --output-on-failure` green, including the `validation` gate and the `gpu` band.
- `hello_triangle-launcher` under `HT_SMOKE` writes the correct-sized PPM;
  `smoke_golden` green (none of this planset may move the golden).
- `sdk_conformance_install` + `sdk_conformance_buildtree` green — the template (migrated in
  Plans 01/03) cooks and builds out-of-tree against the SDK with the new PUBLIC dep.
- The dedupe payoff stated with numbers in the closing commit: lines deleted across the four
  forks vs. the one walker.

### 4. Roadmap pass

- `plans/planset-43/README.md` status column → `done` per plan; the planset entry in
  `plans/README.md` gets its ✅ summary.
- `plans/future/` cross-references checked: any future area that assumed per-consumer JSON
  walkers or the `{ value, name }` MCP shape updates its wording.
