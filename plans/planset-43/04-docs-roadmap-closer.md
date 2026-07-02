# Plan 04 — docs, template co-migration + conformance, roadmap pass

**Goal:** the closer. Documentation catches up with the new posture (one JSON walker,
enums by name, nlohmann PUBLIC, the respelled string forms), the dual-example conformance
band runs end to end, and the planset closes out. Depends on Plans 00a–03.

## What lands

### 1. `CLAUDE.md` updates

- **Root `CLAUDE.md`:** the dependency paragraph — nlohmann/json moves from
  "cooker-only / PRIVATE everywhere" to a PUBLIC dep of `libveng` (joining glm/fmt/ImGui in
  the `include_hygiene` link set); the "Build configurations" section's *"the JSON lives
  entirely in the consumers … so `libveng` gains no JSON dependency"* claim is rewritten to
  the new posture (the engine owns the JSON⇄reflection walker; cooked blobs stay binary and
  the runtime load path still parses no JSON). The mcp paragraph's *"all **PRIVATE** to
  `veng_mcp` … reach neither `libveng` nor a `Veng/Mcp/` public header"* line is corrected —
  nlohmann is now transitively PUBLIC on `veng::mcp` through the `veng::veng` edge, so the
  claim it protects (linking `veng::mcp` alone adds no JSON dep) no longer holds for JSON.
- **`engine/CLAUDE.md`:** the reflection section documents `JsonSerialize.h` beside the
  binary serializer (the hook seam, the merge-write overload, the tolerant-read flag, the
  dotted-path errors) and the enum-by-name convention (`EnumName.h`'s runtime-typed
  functions, exact spellings, hard cut). Its Project section's *"the JSON lives entirely in
  the consumers … so `libveng` gains no JSON dependency"* instance is rewritten to match the
  root file (the same sentence lives in both).
- **`cooker/CLAUDE.md`:** PrefabImporter/LevelImporter as walker call sites (validation and
  entity-remap as hooks); the respelled manifest `"type"`, `"domain"`, and `"compression"`
  vocabularies wherever they're quoted.
- **`editor/CLAUDE.md`:** `PrefabSerialize` and `LevelEditorPanel`'s config round-trip as the
  walker's write inverses (merge-write preserving unknown keys, tolerant read).
- **`mcp/CLAUDE.md`:** the enum output shape change (bare enumerator name, no
  `{ value, name }` object) and the strings-only mutate contract; **and the "JSON-library-free
  public surface" section rewritten** — nlohmann rides in transitively via `veng::veng`, so
  the surface is no longer JSON-library-free and the `mcp_include_hygiene` guard now covers
  httplib only (its header comment in `tests/mcp_include_hygiene.cpp` and the CMake comment
  update with it, per Plan 00a).
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
- **Cook-throughput check:** a before/after `vengc` wall-clock over the full in-tree asset
  set — the generic `std::function`-hook walker replaces five branch-predictable ones, so
  confirm cook time is not measurably worse (it is a batch path, so a small regression is
  acceptable; a large one is a finding). Note in the same pass whether MCP's typical payloads
  make the walker's per-field indirection negligible inside its render-thread-blocking pump
  window.
- The dedupe payoff stated with numbers in the closing commit: lines deleted across the five
  forks vs. the one walker.

### 4. Roadmap pass

- `plans/planset-43/README.md` status column → `done` per plan; the planset entry in
  `plans/README.md` gets its ✅ summary.
- `plans/future/` cross-references checked: any future area that assumed per-consumer JSON
  walkers or the `{ value, name }` MCP shape updates its wording; the reflected Project-model
  hand-parsers (`Cooker.cpp`'s `ParseBuildConfiguration`/`ParseProject`, the editor's
  `ProjectSettingsPanel`) are noted as the remaining unmigrated reflection walk (see the
  planset README's future notes).

## Out of scope

- No further JSON surfaces or spellings migrate here — this plan documents and verifies what
  Plans 00a–03 landed. The Project-model hand-parsers stay deliberately unmigrated (README
  future notes). This plan deliberately omits the "starting point" section the implementation
  plans carry: it is a pure closer with no single source file.
