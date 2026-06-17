# planset-15 — node-based material editor (area 6, sub-area C)

**Phase goal:** deliver the node-based material editor — the headline editor slice and the
**loaded `.vmat` path** the asset system left open. An imnodes graph authors a material; its
compiled output is a `.vmat.json` field list; the cook-on-demand + hot-reload loop previews
it live on a sphere. The reusable graph machinery lands as a **named `libveng_editor`
surface** (`VengEditor/NodeGraph/`) so later editors and game editor modules build on it.

This is [future area 6](../future/README.md#6-editor-application) sub-area C, the
prioritized next editor planset, following sub-area B (editor shell + framework, planset-14).

## Prerequisites

All in place:

- **Editor shell + framework** (planset-14): `EditorPanel`, `EditorRegistry`
  (`RegisterAssetEditor` / `RegisterFieldWidget`), `EditorHost`, the asset browser's
  double-click → `AssetEditorFactory` open flow, and `AssetSourceIndex` (AssetId → source).
- **Cook-on-demand** (planset-14): `EditorHost::RequestCook`, `CookSession`,
  `AssetManager::MountMemory`/`MountHandle`. The `MaterialImporter` already cooks
  `*.vmat.json` in `libveng_cook`, so recooking an authored material is already supported by
  the existing backend — no importer change.
- **Reflection layer** (planset-10/11): `FieldDescriptor`/`FieldClass` and the inspector's
  per-`FieldClass` widgets. Node properties reuse these for *scalar/vector/enum* fields. The
  inspector's `AssetHandle` widget is today a **read-only label** (it shows the id, it cannot
  pick) — this planset adds the actual asset picker (decision 6, plan 05).
- **SceneRenderer + primitives** (planset-7/12/13): `Primitives::Sphere`, `SceneRenderer`,
  `GetOutput`. The live preview renders one sphere with the authored material, exactly the
  `SceneViewportPanel` pattern.
- **imnodes** is fetched (`CMakeLists.txt`, pinned master commit) with its own CMake — which
  already defines an `imnodes` library target — but **not yet linked into any target**
  (`EXCLUDE_FROM_ALL`, fed the `imgui_headers` INTERFACE target for includes). This planset
  links that target into `libveng_editor` (PRIVATE); it adds no source-include aggregation TU
  (that would double-compile `imnodes.cpp`). The runtime never links it.

**Not yet in place — built by this planset (plan 00):** a material's GPU parameters are a
**fixed 32-byte `MaterialData`** with one authored `vec4 Factors`; custom per-material
uniforms are an engine-source change, not data. Plan 00 splits parameters into a fixed
**engine-supplied** block and a variable-size **authored** block so a node graph can author
parameters at all.

## The architecture — layered, with hard dependency ceilings

The design rule that keeps node graphs from rotting: **a lower layer never names a higher
concept.** Topology never says "material"; the model never draws imnodes.

A precursor sits underneath, in the engine itself:

```
Layer 0  Variable material params  the engine-supplied + authored material parameter
         (engine + cooker + shaders)  split. No editor code; the data foundation a node
              │                        graph authors into. Independent of Layers 1–4.
              ▼
```

```
Layer 1  NodeGraph topology    pure data + ops + validation. Generic. Unit-tested,
         (named public surface)  device-free, ImGui-free, knows nothing of "material".
              │
Layer 2  Node catalog          data-driven node TYPES (pins + reflected-struct
         (named public surface)  PROPERTIES); node instances = bytes + connections;
              │                   graph (de)serialization. Still generic.
              │
Layer 3  Material specialization  the material node catalog (TextureSample / Param /
         (editor src)             MaterialOutput / math), the type-compat predicate, and
              │                   CompileMaterialGraph(graph) → .vmat field list +
              │                   the inverse flat-import. The ONLY layer that says
              │                   "material".
              │
Layer 4  MaterialEditorPanel    imnodes canvas (drives Layer 1), node-property
         (editor src)             inspector (reuses Layer-2 field widgets), the live
                                  preview (Layer P), wired to the edit→compile→cook→
                                  hot-reload→preview loop. Owns no model truth.

Layer P  Material preview        P for *preview*: a reusable surface outside the 1→4
         (editor src)             stack — a one-sphere Scene + a SceneRenderer → preview RT
                                  → ImGuiTexture, SetMaterial each edit. Depends only on the
                                  engine — independent of Layers 0–3.
```

## Decisions

0. **Material parameters split into an engine-supplied block and a variable-size authored
   block (plan 00).** The fixed 32-byte `MaterialData` becomes the **engine block** (the
   bindless handle slots the loader patches; fixed `sizeof`, known to `libveng` without
   reflection) plus a separate variable-size **authored block** (`MaterialParams`, the
   scalar/vector uniforms the shader declares, reflected per-shader, byte-addressed at a fixed
   stride). The seam is the existing `CookedMaterialField::Kind` (handle vs. param). This is
   what lets a node graph author parameters; without it a compiled graph has nowhere to put a
   uniform beyond the one `vec4 Factors`.

1. **The generic graph is a named `libveng_editor` surface.** `VengEditor/NodeGraph/`
   (`NodeGraph.h`, `NodeType.h`, serialization). It is reusable by future editors and game
   editor modules — not buried as a material-internal seam. The material specialization
   (Layer 3) consumes it from `editor/src/material/` and is **not** part of the public
   surface.

2. **The topology core is pure and imnodes-free.** No ImGui, no Vulkan, and no mention of
   "material" — so it is unit-tested device-free (the `DecideBarrier`/live-range pattern).
   Only the panel (Layer 4) touches imnodes; the model is a pure projection target.

3. **`NodeType` is data, not a subclass.** A node type is a descriptor — value pins (in/out)
   plus a **reflected property struct** (`vector<FieldDescriptor>`). A node *instance* holds
   an opaque byte buffer matching that struct (walked by the existing reflection serializer
   and inspector widgets, exactly like an ECS component) plus its connections. Adding a node
   type is a catalog entry, never a subclass with virtuals.

4. **Pin types reuse the `TypeId` space; pin-type wraps it.** A `PinType` is
   `{ Value(TypeId), Wildcard }` — data pins carry a builtin leaf `TypeId` (`vec4`, `f32`).
   Connection validity is a domain-supplied `CanConnect(PinType, PinType)` predicate (Layer 1
   takes the hook; the material catalog supplies coercion rules — scalar→vector splat, etc.).
   Acyclicity / direction / arity are generic and owned by Layer 1.

5. **`NodeTypeId` is editor-local, not a global `TypeId`.** Node-type identity is a catalog
   id inside `libveng_editor`; node types are not registered into the runtime `TypeRegistry`
   (no global-id pollution, no minting). Pin data types still reference the existing builtin
   `TypeId`s.

6. **Textures are node *properties*, not wired pins.** A `TextureSample` node has a
   `Texture` property of `FieldClass::AssetHandle` (`AssetHandle<Texture>`), drawn by an asset
   picker and compiled into a `texture` field of the `.vmat`. The inspector's current
   `AssetHandle` widget is a **read-only label**, so this planset **builds the picker** (a
   real combo over candidate `AssetId`s of the right `AssetType`, plan 05) — it is new work,
   not a free reuse. Assets never travel on a link in v1, so Layer 1 stays asset-agnostic. An
   exposed-texture-parameter pin kind is out of scope.

7. **The graph document is embedded in the `.vmat.json`, versioned.** The cooked source stays
   one file per asset (the manifest is `{id, type, source}`). The node graph (nodes, canvas
   positions, property values, connections) lives under an editor-only `"_editor"` key
   carrying a `"version"` integer; the `MaterialImporter` ignores unknown keys (verified — it
   reads only `shaders`/`fields`), and compile **regenerates** the `fields` array. This reuses
   planset-14's "preserve unknown keys" JSON round-trip. Property values serialize to JSON
   through a per-`FieldClass` walker over the node's `FieldDescriptor`s (a new JSON walker, not
   the binary `WriteFields`; see plan 02) — human-diffable in the `.vmat.json`.

8. **Opening a flat `.vmat` synthesizes a graph, non-destructively.** A material with no
   `"_editor"` block imports into a default graph (a `TextureSample`→`Output` per texture
   field, a `Param` per value field). Opening does **not** rewrite the on-disk `fields`: the
   editor regenerates `fields` only on an explicit user edit + save, and a graph whose
   `"version"` exceeds the editor's opens read-only rather than regenerating from a degraded
   parse (plans 02/05). So existing materials open without manual migration and without silent
   corruption.

9. **v1 binds parameters; it does not generate shader source.** The graph wires
   textures/params into an author-provided shader's reflected `MaterialData` — reusing the
   constructed-material path wholesale. Node→Slang codegen is deferred; the node/property
   model is shaped so codegen can layer on without breaking v1 graphs.

10. **No undo/redo this planset.** All model mutation routes through the narrow Layer-1
    command vocabulary (`AddNode`/`RemoveNode`/`Connect`/`Disconnect`/`SetProperty`/
    `MoveNode`) so the future command stack is a thin wrapper, not a refactor — but the stack
    itself is out of scope.

## Scope

| In scope | Out of scope |
|---|---|
| Variable-size material params: engine-supplied + authored block split (plan 00) | Node→Slang shader-graph codegen (deferred) |
| `VengEditor/NodeGraph/` named surface: topology core + data-driven catalog + serialization | Wired asset pins / exposed texture parameters |
| Material node catalog (`TextureSample`, `Param`, `MaterialOutput`, basic math), type-compat | Undo/redo command stack |
| `CompileMaterialGraph` (graph → `.vmat` field list) + flat-`.vmat`→graph import | Multi-viewport OS windows |
| `MaterialEditorPanel`: imnodes canvas, node-property inspector, live preview | Scene editor (sub-area D — its own planset) |
| Reusable material preview surface (sphere + `SceneRenderer` → RT) | New cooked assets / new `AssetId`s (brick.vmat reused) |
| The inspector `AssetHandle` **asset picker** (new; shared by entity + node inspectors) | Custom game-defined node types (the seam exists; no sample) |
| imnodes library target linked PRIVATE into `libveng_editor` (runtime never links it) | — |
| hello-triangle: open `brick.vmat.json` in the node editor, edit, live preview | — |
| Update `plans/README.md`, `plans/future/README.md`, `CLAUDE.md` | — |

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 00 | [Variable-size material params](00-material-params.md) | Layer 0 (**parallel**): split material parameters into a fixed engine-supplied `MaterialData` block + a variable-size authored `MaterialParams` block (second SSBO, byte-addressed); reflect both; `Material::GetFields()`. Engine + cooker + shaders. | done |
| 01 | [NodeGraph topology core](01-nodegraph-core.md) | Layer 1 (**parallel**): `NodeGraph`, `NodeId`/`Link`/`PinType`, mutation vocabulary, validation (direction/arity/acyclicity), the `CanConnect` hook. Named public surface. Pure, device-free, unit-tested. | done |
| 02 | [Node catalog + serialization](02-node-catalog-serialize.md) | Layer 2: data-driven `NodeType` (pins + reflected property struct), node-instance byte storage, graph (de)serialization to/from a JSON object via a per-`FieldClass` JSON walker. Generic. | done |
| 03 | [Material catalog + compile](03-material-catalog-compile.md) | Layer 3: material node types, the coercion `CanConnect`, `CompileMaterialGraph` → `.vmat` fields, flat-`.vmat`→graph import. Material-specific, in editor src. | done |
| 04 | [Material preview surface](04-material-preview.md) | Layer P (**parallel**): a reusable one-sphere `Scene` + `SceneRenderer` → preview RT → `Ref<ImGuiTexture>`, `SetMaterial` per edit, re-fetch on hot-reload. Depends only on the engine. | done |
| 05 | [MaterialEditorPanel + integration](05-material-editor-panel.md) | Layer 4: imnodes canvas over the graph, property inspector (the new asset picker), wire compile→cook→hot-reload→preview, register for `AssetType::Material`, link the imnodes target into `libveng_editor`, hello-triangle migration. | done |
| 06 | [Docs + roadmap re-cut](06-docs-roadmap.md) | `future/editor.md` sub-area C → delivered; `future/README.md` + `plans/README.md` + `CLAUDE.md`. No code. | done |

## Dependency analysis & parallel dispatch

The chain is deliberately split so the params precursor (00) and the preview surface (04) run
**concurrently** with the topology→catalog chain (01→02), since each depends only on the
shipped engine. 03 (compile) is the join that needs both the catalog (02) **and** the variable
params (00); 05 (panel) needs 03 and 04.

```
        ┌──────────────────────────── critical path ───────────────────────────┐
01 core ──► 02 catalog/serialize ──────────────► 03 material compile ──► 05 panel ──► 06 docs
00 material params ──────────────────────────────┘                       ▲
  (engine/cooker/shaders; no node graph)                                 │
04 material preview  ────────────────────────────────────────────────────┘
  (no dependency on 00–03; depends only on SceneRenderer/Primitives/Material)
```

| Wave | Dispatch | Depends on | Notes |
|---|---|---|---|
| 1 | **00** ‖ **01** ‖ **04** | engine only | Three agents in parallel: params precursor (engine/cooker/shaders), topology core (`VengEditor/NodeGraph/`), preview surface (`editor/src/material/MaterialPreview.*`). Disjoint **product** files; they share two append-only CMake seams (see the file-contention note). |
| 2 | **02** | 01 | 00 and 04 may still be in flight. |
| 3 | **03** | 02 **and** 00 | The material specialization — authors params into the block 00 added, reads `Material::GetFields()`. |
| 4 | **05** | 03 **and** 04 | The integration join — needs both branches landed. |
| 5 | **06** | all | Docs, no code. |

- **Parallelizable:** plans **00, 01, 04** (Wave 1). 00 is engine/cooker/shaders only; 01 is
  the editor topology surface; 04 is a clean engine-only module (`MaterialPreview`) testable
  against the cooked brick material with no node graph present. All three hide under the
  critical path.
- **Strictly sequential:** 01 → 02 → 03 (each builds on the prior layer's types), with 03 also
  gated on 00; then 05 (the join), then 06.
- **Critical path:** 01 → 02 → 03 → 05 → 06 (five plans). With 00 and 04 dispatched in Wave 1,
  the params and preview work cost no extra wall-clock time.
- **File-contention note.** Product code is disjoint: 00 touches `engine/`, `cooker/`,
  `assetformat/`, and the `material_data.slang` copies under `examples/` + `tests/`; 01–02 add
  new files under `VengEditor/NodeGraph/`; 03 and 04 both land under `editor/src/material/` but in
  disjoint files; 05 owns `MaterialEditorPanel`, the `EditorHost`/`AssetBrowserPanel` wiring, and
  the `InspectorPanel.{h,cpp}` + `AssetSourceIndex` changes (factoring `DrawFieldWidget` out and
  adding the type-filtered enumeration for the asset picker). **Two CMake files are shared
  append-only seams, not disjoint:**
  - `editor/CMakeLists.txt` lists the `libveng_editor` sources **explicitly** (no glob), so every
    editor plan that adds a `.cpp` to the library appends to it — 01/02 (`VengEditor/NodeGraph/`),
    03/04 (`editor/src/material/`), 05 (`MaterialEditorPanel` + the imnodes target link).
  - the root `CMakeLists.txt` hosts every test target: 01 adds `veng_editor_unit` and extends
    `veng_include_hygiene` to cover the `VengEditor/` headers; 04 adds its GPU test target (which
    must link `libveng_editor`, as no existing GPU target does); 03's cook-through test links
    `libveng_editor` into the cooker suite.

  Each plan's edit to these two files is a non-overlapping append (a source line, a test target),
  so parallel worktrees merge cleanly — but they are genuinely shared, and the integration step
  must union them rather than assume no conflict.

## Process & conventions

Same cadence as every planset: implement → migrate `examples/hello-triangle` in the same
pass → verify (clean build, `ctest` green, smoke PPM correct size + exit 0) → update this
table → one commit per plan.

- **`libveng_editor` boundary holds.** The `NodeGraph` public surface pulls in only libveng
  vocabulary + `FieldDescriptor`; no Vulkan, no GLFW, no imnodes in a public header. imnodes
  is a src-only dependency of the panel. `include_hygiene` stays green.
- **The runtime never links imnodes.** imnodes' own library target is linked PRIVATE into
  `libveng_editor` only; `libveng` and `libgame` are untouched.
- **The smoke golden is unchanged.** The editor is a separate executable; the launcher PPM
  is unaffected.
- **Contract comments are present-tense facts** — no plan citations, no "future work"
  phrasing (CLAUDE.md).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.

## On completion

veng has a working visual material authoring loop: launch `hello_triangle-editor`,
double-click `brick.vmat`, see its graph (a texture sample feeding the albedo output, a
factors param), wire and tweak nodes, set the albedo texture through the new asset picker, and
watch a live-previewed sphere update as the material recooks off-thread — then save, the
graph embedded under a versioned `"_editor"` key, the `fields` array regenerated. Materials
now carry **variable authored parameters** (plan 00) — a node graph is no longer capped at the
one `vec4 Factors` — over a fixed engine-supplied handle block. The **loaded `.vmat` path** is
closed end-to-end, and the generic `VengEditor/NodeGraph/` surface is in place for the scene
editor (sub-area D) and game editor modules to build on.
</content>
</invoke>
