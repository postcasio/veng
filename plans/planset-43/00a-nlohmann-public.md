# Plan 00a — nlohmann/json PUBLIC on `libveng`

**Goal:** the build-system half of the foundation, landed and verified on its own before any
new code depends on it. nlohmann/json becomes a PUBLIC dependency of `libveng` across all
three consumption modes (in-tree, build-tree, install-prefix), the now-redundant PRIVATE
links are cleaned up, and the `mcp_include_hygiene` guard is re-scoped to reflect that mcp's
public surface is no longer JSON-library-free. No engine header consumes JSON yet — this plan
is purely the dependency-surface change, so its risk (the SDK export/install across three
modes) is isolated from the walker code (Plan 00b). Depends on nothing.

## The starting point

- nlohmann/json is FetchContent-pinned at the top level and linked PRIVATE by cooker
  (`veng_cook_objs` has it PUBLIC internally), editor, graph, and mcp; `libveng` has **no**
  JSON dependency (`target_link_libraries(veng PUBLIC glm::glm fmt::fmt veng::assetpack)`),
  and `include_hygiene` compiles public headers against PUBLIC deps only.
- The nlohmann `FetchContent_Declare`/`FetchContent_MakeAvailable` block currently runs
  *after* `add_subdirectory(assetpack)`/`add_subdirectory(engine)` (it sits in the
  graph/cooker preamble of the root `CMakeLists.txt`).
- `veng_mcp` links `veng::veng` PUBLIC and nlohmann PRIVATE; `tests/mcp_include_hygiene.cpp`
  links `veng::mcp` alone to prove `Veng/Mcp/` public headers pull in neither nlohmann nor
  httplib.

## What lands

- `target_link_libraries(veng PUBLIC nlohmann_json::nlohmann_json)`. **Required reordering:**
  the nlohmann FetchContent block must move **before** `add_subdirectory(assetpack)`/
  `add_subdirectory(engine)`, or the `nlohmann_json` target does not yet exist when
  `engine/CMakeLists.txt` names it and configure fails with an unknown-target error. This is
  not conditional.
- **SDK export/install:** the installed `veng-config.cmake` gains
  `find_dependency(nlohmann_json)`, and the install prefix carries nlohmann (enable the
  FetchContent'd project's install — `JSON_Install` — so the SDK is self-contained, matching
  how the other exported PUBLIC deps resolve). The build-tree mode already has the target in
  scope; verify the exported `vengTargets` resolve it there too.
- `include_hygiene` needs no change in intent — nlohmann simply joins glm/fmt/ImGui in the
  PUBLIC link set it compiles against.
- Consumers' now-redundant PRIVATE links (editor, mcp, graph, cooker) are dropped where the
  transitive PUBLIC edge covers them; comments in those CMakeLists that assert "nlohmann
  stays PRIVATE / never reaches a public header" are updated to the new posture.
- **`mcp_include_hygiene` re-scoped.** Once `veng` carries nlohmann PUBLIC, `veng::mcp`
  inherits nlohmann's include path transitively (through its `PUBLIC veng::veng` link), so
  the test can no longer distinguish "an Mcp header leaks nlohmann" from "nlohmann always
  rides along via `veng::veng`" — its JSON half stops guarding. Narrow its stated contract
  (and the header comment in `tests/mcp_include_hygiene.cpp` + the CMake comment) to
  **httplib-only**; the JSON-free-surface prose in `mcp/CLAUDE.md` and root `CLAUDE.md` is
  rewritten in Plan 04.

## Verification

- `build-debug` clean configure + build with the new PUBLIC dep; `include_hygiene` green;
  `mcp_include_hygiene` green under its narrowed (httplib-only) contract.
- **Acceptance:** `sdk_conformance_install` and `sdk_conformance_buildtree` green — all three
  consumption modes resolve the new dependency. This is the whole point of landing 00a
  first: the three-mode SDK resolution is the fiddly, slow-to-iterate risk, proven before the
  walker rides on top of it.

## Out of scope

- `Veng/Reflection/JsonSerialize.h`, the runtime-typed enum functions, and the unit tests
  (Plan 00b) — no engine header consumes JSON in this plan; the PUBLIC link simply makes the
  dependency available. The five forks still compile and run unchanged.
- The `mcp/CLAUDE.md` / root `CLAUDE.md` prose rewrite (Plan 04) — this plan changes only the
  test's contract + inline comments, not the guide prose.
